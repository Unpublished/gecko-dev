/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossProcessPaint.h"

#include "mozilla/dom/ContentProcessManager.h"
#include "mozilla/dom/ImageBitmap.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/PWindowGlobalParent.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/WindowGlobalParent.h"
#include "mozilla/dom/WindowGlobalChild.h"
#include "mozilla/dom/WindowGlobalActorsBinding.h"
#include "mozilla/gfx/DrawEventRecorder.h"
#include "mozilla/gfx/InlineTranslator.h"
#include "mozilla/Logging.h"
#include "mozilla/PresShell.h"

#include "gfxPlatform.h"

#include "nsContentUtils.h"
#include "nsGlobalWindowInner.h"
#include "nsIDocShell.h"
#include "nsPresContext.h"

static mozilla::LazyLogModule gCrossProcessPaintLog("CrossProcessPaint");
static mozilla::LazyLogModule gPaintFragmentLog("PaintFragment");

#define CPP_LOG(msg, ...) \
  MOZ_LOG(gCrossProcessPaintLog, LogLevel::Debug, (msg, ##__VA_ARGS__))
#define PF_LOG(msg, ...) \
  MOZ_LOG(gPaintFragmentLog, LogLevel::Debug, (msg, ##__VA_ARGS__))

namespace mozilla {
namespace gfx {

using namespace mozilla::ipc;

/// The minimum scale we allow tabs to be rasterized at.
static const float kMinPaintScale = 0.05f;

/* static */
PaintFragment PaintFragment::Record(dom::BrowsingContext* aBc,
                                    const Maybe<IntRect>& aRect, float aScale,
                                    nscolor aBackgroundColor,
                                    CrossProcessPaintFlags aFlags) {
  nsIDocShell* ds = aBc->GetDocShell();
  if (!ds) {
    PF_LOG("Couldn't find docshell.\n");
    return PaintFragment{};
  }

  RefPtr<nsPresContext> presContext = ds->GetPresContext();
  if (!presContext) {
    PF_LOG("Couldn't find PresContext.\n");
    return PaintFragment{};
  }

  IntRect rect;
  if (!aRect) {
    nsCOMPtr<nsIWidget> widget =
        nsContentUtils::WidgetForDocument(presContext->Document());

    // TODO: Apply some sort of clipping to visible bounds here (Bug 1562720)
    LayoutDeviceIntRect boundsDevice = widget->GetBounds();
    boundsDevice.MoveTo(0, 0);
    nsRect boundsAu = LayoutDevicePixel::ToAppUnits(
        boundsDevice, presContext->AppUnitsPerDevPixel());
    rect = gfx::RoundedOut(CSSPixel::FromAppUnits(boundsAu).ToUnknownRect());
  } else {
    rect = *aRect;
  }

  if (rect.IsEmpty()) {
    // TODO: Should we return an empty surface here?
    PF_LOG("Empty rect to paint.\n");
    return PaintFragment{};
  }

  IntSize surfaceSize = rect.Size();
  surfaceSize.width *= aScale;
  surfaceSize.height *= aScale;

  CPP_LOG(
      "Recording "
      "[browsingContext=%p, "
      "rect=(%d, %d) x (%d, %d), "
      "scale=%f, "
      "color=(%u, %u, %u, %u)]\n",
      aBc, rect.x, rect.y, rect.width, rect.height, aScale,
      NS_GET_R(aBackgroundColor), NS_GET_G(aBackgroundColor),
      NS_GET_B(aBackgroundColor), NS_GET_A(aBackgroundColor));

  // Check for invalid sizes
  if (surfaceSize.width <= 0 || surfaceSize.height <= 0 ||
      !Factory::CheckSurfaceSize(surfaceSize)) {
    PF_LOG("Invalid surface size of (%d x %d).\n", surfaceSize.width,
           surfaceSize.height);
    return PaintFragment{};
  }

  // Flush any pending notifications
  nsContentUtils::FlushLayoutForTree(ds->GetWindow());

  // Initialize the recorder
  SurfaceFormat format = SurfaceFormat::B8G8R8A8;
  RefPtr<DrawTarget> referenceDt = Factory::CreateDrawTarget(
      gfxPlatform::GetPlatform()->GetSoftwareBackend(), IntSize(1, 1), format);

  // TODO: This may OOM crash if the content is complex enough
  RefPtr<DrawEventRecorderMemory> recorder =
      MakeAndAddRef<DrawEventRecorderMemory>(nullptr);
  RefPtr<DrawTarget> dt = Factory::CreateRecordingDrawTarget(
      recorder, referenceDt, IntRect(IntPoint(0, 0), surfaceSize));

  RenderDocumentFlags renderDocFlags = RenderDocumentFlags::None;
  if (!(aFlags & CrossProcessPaintFlags::DrawView)) {
    renderDocFlags = (RenderDocumentFlags::IgnoreViewportScrolling |
                      RenderDocumentFlags::ResetViewportScrolling |
                      RenderDocumentFlags::DocumentRelative);
  }

  // Perform the actual rendering
  {
    nsRect r(nsPresContext::CSSPixelsToAppUnits(rect.x),
             nsPresContext::CSSPixelsToAppUnits(rect.y),
             nsPresContext::CSSPixelsToAppUnits(rect.width),
             nsPresContext::CSSPixelsToAppUnits(rect.height));

    RefPtr<gfxContext> thebes = gfxContext::CreateOrNull(dt);
    thebes->SetMatrix(Matrix::Scaling(aScale, aScale));
    RefPtr<PresShell> presShell = presContext->PresShell();
    Unused << presShell->RenderDocument(r, renderDocFlags, aBackgroundColor,
                                        thebes);
  }

  if (!recorder->mOutputStream.mValid) {
    return PaintFragment{};
  }

  ByteBuf recording = ByteBuf((uint8_t*)recorder->mOutputStream.mData,
                              recorder->mOutputStream.mLength,
                              recorder->mOutputStream.mCapacity);
  recorder->mOutputStream.mData = nullptr;
  recorder->mOutputStream.mLength = 0;
  recorder->mOutputStream.mCapacity = 0;

  return PaintFragment{
      surfaceSize,
      std::move(recording),
      std::move(recorder->TakeDependentSurfaces()),
  };
}

bool PaintFragment::IsEmpty() const {
  return !mRecording.mData || mRecording.mLen == 0 || mSize == IntSize(0, 0);
}

PaintFragment::PaintFragment(IntSize aSize, ByteBuf&& aRecording,
                             nsTHashtable<nsUint64HashKey>&& aDependencies)
    : mSize(aSize),
      mRecording(std::move(aRecording)),
      mDependencies(std::move(aDependencies)) {}

static dom::TabId GetTabId(dom::WindowGlobalParent* aWGP) {
  // There is no unique TabId for a given WindowGlobalParent, as multiple
  // WindowGlobalParents share the same PBrowser actor. However, we only
  // ever queue one paint per PBrowser by just using the current
  // WindowGlobalParent for a PBrowser. So we can interchange TabId and
  // WindowGlobalParent when dealing with resolving surfaces.
  RefPtr<dom::BrowserParent> browserParent = aWGP->GetBrowserParent();
  return browserParent ? browserParent->GetTabId() : dom::TabId(0);
}

/* static */
bool CrossProcessPaint::Start(dom::WindowGlobalParent* aRoot,
                              const dom::DOMRect* aRect, float aScale,
                              nscolor aBackgroundColor,
                              CrossProcessPaintFlags aFlags,
                              dom::Promise* aPromise) {
  MOZ_RELEASE_ASSERT(XRE_IsParentProcess());
  aScale = std::max(aScale, kMinPaintScale);

  CPP_LOG(
      "Starting paint. "
      "[wgp=%p, "
      "scale=%f, "
      "color=(%u, %u, %u, %u)]\n",
      aRoot, aScale, NS_GET_R(aBackgroundColor), NS_GET_G(aBackgroundColor),
      NS_GET_B(aBackgroundColor), NS_GET_A(aBackgroundColor));

  Maybe<IntRect> rect;
  if (aRect) {
    rect =
        Some(IntRect::RoundOut((float)aRect->X(), (float)aRect->Y(),
                               (float)aRect->Width(), (float)aRect->Height()));
  }

  if (rect && rect->IsEmpty()) {
    return false;
  }

  dom::TabId rootId = GetTabId(aRoot);

  RefPtr<CrossProcessPaint> resolver = new CrossProcessPaint(aScale, rootId);
  RefPtr<CrossProcessPaint::ResolvePromise> promise;

  if (aRoot->IsInProcess()) {
    RefPtr<dom::WindowGlobalChild> childActor = aRoot->GetChildActor();
    if (!childActor) {
      return false;
    }

    // `BrowsingContext()` cannot be nullptr.
    RefPtr<dom::BrowsingContext> bc = childActor->BrowsingContext();

    promise = resolver->Init();
    resolver->mPendingFragments += 1;
    resolver->ReceiveFragment(
        aRoot,
        PaintFragment::Record(bc, rect, aScale, aBackgroundColor, aFlags));
  } else {
    promise = resolver->Init();
    resolver->QueuePaint(aRoot, rect, aBackgroundColor, aFlags);
  }

  promise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promise = RefPtr{aPromise}, rootId](ResolvedFragmentMap&& aFragments) {
        RefPtr<RecordedDependentSurface> root = aFragments.Get(rootId);
        CPP_LOG("Resolved all fragments.\n");

        // Create the destination draw target
        RefPtr<DrawTarget> drawTarget =
            gfxPlatform::GetPlatform()->CreateOffscreenContentDrawTarget(
                root->mSize, SurfaceFormat::B8G8R8A8);
        if (!drawTarget || !drawTarget->IsValid()) {
          CPP_LOG("Couldn't create (%d x %d) surface for fragment %" PRIu64
                  ".\n",
                  root->mSize.width, root->mSize.height, (uint64_t)rootId);
          promise->MaybeReject(NS_ERROR_FAILURE);
          return;
        }

        // Translate the recording using our child tabs
        {
          InlineTranslator translator(drawTarget, nullptr);
          translator.SetDependentSurfaces(&aFragments);
          if (!translator.TranslateRecording((char*)root->mRecording.mData,
                                             root->mRecording.mLen)) {
            CPP_LOG("Couldn't translate recording for fragment %" PRIu64 ".\n",
                    (uint64_t)rootId);
            promise->MaybeReject(NS_ERROR_FAILURE);
            return;
          }
        }

        RefPtr<SourceSurface> snapshot = drawTarget->Snapshot();
        if (!snapshot) {
          promise->MaybeReject(NS_ERROR_FAILURE);
          return;
        }

        ErrorResult rv;
        RefPtr<dom::ImageBitmap> bitmap =
            dom::ImageBitmap::CreateFromSourceSurface(
                promise->GetParentObject(), snapshot, rv);

        if (!rv.Failed()) {
          CPP_LOG("Success, fulfilling promise.\n");
          promise->MaybeResolve(bitmap);
        } else {
          CPP_LOG("Couldn't create ImageBitmap for SourceSurface.\n");
          promise->MaybeReject(std::move(rv));
        }
      },
      [promise = RefPtr{aPromise}](const nsresult& aRv) {
        promise->MaybeReject(aRv);
      });

  return true;
}

/* static */
RefPtr<CrossProcessPaint::ResolvePromise> CrossProcessPaint::Start(
    nsTHashtable<nsUint64HashKey>&& aDependencies) {
  MOZ_ASSERT(!aDependencies.IsEmpty());
  RefPtr<CrossProcessPaint> resolver =
      new CrossProcessPaint(1.0, dom::TabId(0));

  RefPtr<CrossProcessPaint::ResolvePromise> promise = resolver->Init();

  PaintFragment rootFragment;
  rootFragment.mDependencies = std::move(aDependencies);

  resolver->QueueDependencies(rootFragment.mDependencies);
  resolver->mReceivedFragments.Put(dom::TabId(0), std::move(rootFragment));

  resolver->MaybeResolve();

  return promise;
}

CrossProcessPaint::CrossProcessPaint(float aScale, dom::TabId aRoot)
    : mRoot{aRoot}, mScale{aScale}, mPendingFragments{0} {}

CrossProcessPaint::~CrossProcessPaint() = default;

void CrossProcessPaint::ReceiveFragment(dom::WindowGlobalParent* aWGP,
                                        PaintFragment&& aFragment) {
  if (IsCleared()) {
    CPP_LOG("Ignoring fragment from %p.\n", aWGP);
    return;
  }

  dom::TabId surfaceId = GetTabId(aWGP);

  MOZ_ASSERT(mPendingFragments > 0);
  MOZ_ASSERT(!mReceivedFragments.GetValue(surfaceId));

  // Double check our invariants to protect against a compromised content
  // process
  if (mPendingFragments == 0 || mReceivedFragments.GetValue(surfaceId) ||
      aFragment.IsEmpty()) {
    CPP_LOG("Dropping invalid fragment from %p.\n", aWGP);
    LostFragment(aWGP);
    return;
  }

  CPP_LOG("Receiving fragment from %p(%" PRIu64 ").\n", aWGP,
          (uint64_t)surfaceId);

  // Queue paints for child tabs
  QueueDependencies(aFragment.mDependencies);

  mReceivedFragments.Put(surfaceId, std::move(aFragment));
  mPendingFragments -= 1;

  // Resolve this paint if we have received all pending fragments
  MaybeResolve();
}

void CrossProcessPaint::LostFragment(dom::WindowGlobalParent* aWGP) {
  if (IsCleared()) {
    CPP_LOG("Ignoring lost fragment from %p.\n", aWGP);
    return;
  }

  Clear(NS_ERROR_LOSS_OF_SIGNIFICANT_DATA);
}

void CrossProcessPaint::QueueDependencies(
    const nsTHashtable<nsUint64HashKey>& aDependencies) {
  for (auto iter = aDependencies.ConstIter(); !iter.Done(); iter.Next()) {
    auto dependency = dom::TabId(iter.Get()->GetKey());

    // Get the current WindowGlobalParent of the remote browser that was marked
    // as a dependency
    dom::ContentProcessManager* cpm =
        dom::ContentProcessManager::GetSingleton();
    dom::ContentParentId cpId = cpm->GetTabProcessId(dependency);
    RefPtr<dom::BrowserParent> browser =
        cpm->GetBrowserParentByProcessAndTabId(cpId, dependency);
    if (!browser) {
      CPP_LOG("Skipping dependency %" PRIu64 "with no current BrowserParent.\n",
              (uint64_t)dependency);
      continue;
    }
    RefPtr<dom::WindowGlobalParent> wgp =
        browser->GetBrowsingContext()->GetCurrentWindowGlobal();

    if (!wgp) {
      CPP_LOG("Skipping dependency %" PRIu64 "with no current WGP.\n",
              (uint64_t)dependency);
      continue;
    }

    // TODO: Apply some sort of clipping to visible bounds here (Bug 1562720)
    QueuePaint(wgp, Nothing());
  }
}

void CrossProcessPaint::QueuePaint(dom::WindowGlobalParent* aWGP,
                                   const Maybe<IntRect>& aRect,
                                   nscolor aBackgroundColor,
                                   CrossProcessPaintFlags aFlags) {
  MOZ_ASSERT(!mReceivedFragments.GetValue(GetTabId(aWGP)));

  CPP_LOG("Queueing paint for %p.\n", aWGP);

  aWGP->DrawSnapshotInternal(this, aRect, mScale, aBackgroundColor,
                             (uint32_t)aFlags);
  mPendingFragments += 1;
}

void CrossProcessPaint::Clear(nsresult aStatus) {
  mPendingFragments = 0;
  mReceivedFragments.Clear();
  mPromise.RejectIfExists(aStatus, __func__);
}

bool CrossProcessPaint::IsCleared() const { return mPromise.IsEmpty(); }

void CrossProcessPaint::MaybeResolve() {
  // Don't do anything if we aren't ready, experienced an error, or already
  // resolved this paint
  if (IsCleared() || mPendingFragments > 0) {
    CPP_LOG("Not ready to resolve yet, have %u fragments left.\n",
            mPendingFragments);
    return;
  }

  CPP_LOG("Starting to resolve fragments.\n");

  // Resolve the paint fragments from the bottom up
  ResolvedFragmentMap resolved;
  {
    nsresult rv = ResolveInternal(mRoot, &resolved);
    if (NS_FAILED(rv)) {
      CPP_LOG("Couldn't resolve.\n");
      Clear(rv);
      return;
    }
  }

  CPP_LOG("Resolved all fragments.\n");

  mPromise.ResolveIfExists(std::move(resolved), __func__);
  Clear(NS_OK);
}

nsresult CrossProcessPaint::ResolveInternal(dom::TabId aTabId,
                                            ResolvedFragmentMap* aResolved) {
  // We should not have resolved this paint already
  MOZ_ASSERT(!aResolved->GetWeak(aTabId));

  CPP_LOG("Resolving fragment %" PRIu64 ".\n", (uint64_t)aTabId);

  Maybe<PaintFragment> fragment = mReceivedFragments.Extract(aTabId);
  if (!fragment) {
    return NS_ERROR_LOSS_OF_SIGNIFICANT_DATA;
  }

  // Rasterize all the dependencies first so that we can resolve this fragment
  for (auto iter = fragment->mDependencies.Iter(); !iter.Done(); iter.Next()) {
    auto dependency = dom::TabId(iter.Get()->GetKey());

    nsresult rv = ResolveInternal(dependency, aResolved);
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  RefPtr<RecordedDependentSurface> surface = new RecordedDependentSurface{
      fragment->mSize, std::move(fragment->mRecording)};
  aResolved->Put(aTabId, std::move(surface));
  return NS_OK;
}

}  // namespace gfx
}  // namespace mozilla
