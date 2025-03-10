/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

#include "ConnectionHandle.h"
#include "DnsAndConnectSocket.h"
#include "nsHttpConnection.h"
#include "nsIDNSRecord.h"
#include "nsIDNSService.h"
#include "nsQueryObject.h"
#include "nsURLHelper.h"
#include "mozilla/Components.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/SyncRunnable.h"

// Log on level :5, instead of default :4.
#undef LOG
#define LOG(args) LOG5(args)
#undef LOG_ENABLED
#define LOG_ENABLED() LOG5_ENABLED()

namespace mozilla {
namespace net {

//////////////////////// DnsAndConnectSocket
NS_IMPL_ADDREF(DnsAndConnectSocket)
NS_IMPL_RELEASE(DnsAndConnectSocket)

NS_INTERFACE_MAP_BEGIN(DnsAndConnectSocket)
  NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
  NS_INTERFACE_MAP_ENTRY(nsIOutputStreamCallback)
  NS_INTERFACE_MAP_ENTRY(nsITransportEventSink)
  NS_INTERFACE_MAP_ENTRY(nsIInterfaceRequestor)
  NS_INTERFACE_MAP_ENTRY(nsITimerCallback)
  NS_INTERFACE_MAP_ENTRY(nsINamed)
  NS_INTERFACE_MAP_ENTRY(nsIDNSListener)
  NS_INTERFACE_MAP_ENTRY_CONCRETE(DnsAndConnectSocket)
NS_INTERFACE_MAP_END

DnsAndConnectSocket::DnsAndConnectSocket(ConnectionEntry* ent,
                                         nsAHttpTransaction* trans,
                                         uint32_t caps, bool speculative,
                                         bool isFromPredictor, bool urgentStart)
    : mTransaction(trans),
      mPrimaryTransport(false),
      mCaps(caps),
      mSpeculative(speculative),
      mUrgentStart(urgentStart),
      mIsFromPredictor(isFromPredictor),
      mEnt(ent),
      mBackupTransport(true) {
  MOZ_ASSERT(ent && trans, "constructor with null arguments");
  LOG(("Creating DnsAndConnectSocket [this=%p trans=%p ent=%s key=%s]\n", this,
       trans, ent->mConnInfo->Origin(), ent->mConnInfo->HashKey().get()));

  mIsHttp3 = mEnt->mConnInfo->IsHttp3();
  if (speculative) {
    Telemetry::AutoCounter<Telemetry::HTTPCONNMGR_TOTAL_SPECULATIVE_CONN>
        totalSpeculativeConn;
    ++totalSpeculativeConn;

    if (isFromPredictor) {
      Telemetry::AutoCounter<Telemetry::PREDICTOR_TOTAL_PRECONNECTS_CREATED>
          totalPreconnectsCreated;
      ++totalPreconnectsCreated;
    }
  }

  MOZ_ASSERT(mEnt);
}

DnsAndConnectSocket::~DnsAndConnectSocket() {
  LOG(("Destroying DnsAndConnectSocket [this=%p]\n", this));
  MOZ_ASSERT(!mPrimaryTransport.mSocketTransport);
  MOZ_ASSERT(!mBackupTransport.mSocketTransport);
  MOZ_ASSERT(mState == DnsAndSocketState::DONE);

  if (mEnt) {
    bool inqueue = mEnt->RemoveDnsAndConnectSocket(this);
    LOG((
        "Destroying DnsAndConnectSocket was in the HalfOpenList=%d [this=%p]\n",
        inqueue, this));
  }
}

nsresult DnsAndConnectSocket::Init() {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  MOZ_ASSERT(mEnt);
  MOZ_ASSERT(mState == DnsAndSocketState::INIT);

  if (mEnt->mConnInfo->GetRoutedHost().IsEmpty()) {
    mPrimaryTransport.mHost = mEnt->mConnInfo->GetOrigin();
    mBackupTransport.mHost = mEnt->mConnInfo->GetOrigin();
  } else {
    mPrimaryTransport.mHost = mEnt->mConnInfo->GetRoutedHost();
    mBackupTransport.mHost = mEnt->mConnInfo->GetRoutedHost();
  }

  CheckProxyConfig();

  if (!mSkipDnsResolution) {
    nsresult rv = SetupDnsFlags();
    NS_ENSURE_SUCCESS(rv, rv);
  }
  return SetupEvent(SetupEvents::INIT_EVENT);
}

void DnsAndConnectSocket::CheckProxyConfig() {
  const nsHttpConnectionInfo* ci = mEnt->mConnInfo;

  if (ci->ProxyInfo()) {
    nsCOMPtr<nsProxyInfo> proxyInfo = ci->ProxyInfo();
    nsAutoCString proxyType(proxyInfo->Type());

    bool proxyTransparent = false;
    if (proxyType.EqualsLiteral("socks") || proxyType.EqualsLiteral("socks4")) {
      proxyTransparent = true;
      if (proxyInfo->Flags() & nsIProxyInfo::TRANSPARENT_PROXY_RESOLVES_HOST) {
        mProxyTransparentResolvesHost = true;
      }
    }

    if (mProxyTransparentResolvesHost) {
      // Name resolution is done on the server side.  Just pretend
      // client resolution is complete, this will get picked up later.
      // since we don't need to do DNS now, we bypass the resolving
      // step by initializing mNetAddr to an empty address, but we
      // must keep the port. The SOCKS IO layer will use the hostname
      // we send it when it's created, rather than the empty address
      // we send with the connect call.
      mPrimaryTransport.mSkipDnsResolution = true;
      mBackupTransport.mSkipDnsResolution = true;
      mSkipDnsResolution = true;
    }

    if (!proxyTransparent && !proxyInfo->Host().IsEmpty()) {
      mProxyNotTransparent = true;
      mPrimaryTransport.mHost = proxyInfo->Host();
      mBackupTransport.mHost = proxyInfo->Host();
    }
  }
}

nsresult DnsAndConnectSocket::SetupDnsFlags() {
  LOG(("DnsAndConnectSocket::SetupDnsFlags [this=%p] ", this));

  uint32_t dnsFlags = 0;
  bool disableIpv6ForBackup = false;
  if (mCaps & NS_HTTP_REFRESH_DNS) {
    dnsFlags = nsIDNSService::RESOLVE_BYPASS_CACHE;
  }
  if (mCaps & NS_HTTP_DISABLE_IPV4) {
    dnsFlags |= nsIDNSService::RESOLVE_DISABLE_IPV4;
  } else if (mCaps & NS_HTTP_DISABLE_IPV6) {
    dnsFlags |= nsIDNSService::RESOLVE_DISABLE_IPV6;
  } else if (mEnt->PreferenceKnown()) {
    if (mEnt->mPreferIPv6) {
      dnsFlags |= nsIDNSService::RESOLVE_DISABLE_IPV4;
    } else if (mEnt->mPreferIPv4) {
      dnsFlags |= nsIDNSService::RESOLVE_DISABLE_IPV6;
    }
    mPrimaryTransport.mRetryWithDifferentIPFamily = true;
    mBackupTransport.mRetryWithDifferentIPFamily = true;
  } else if (gHttpHandler->FastFallbackToIPv4()) {
    // For backup connections, we disable IPv6. That's because some users have
    // broken IPv6 connectivity (leading to very long timeouts), and disabling
    // IPv6 on the backup connection gives them a much better user experience
    // with dual-stack hosts, though they still pay the 250ms delay for each new
    // connection. This strategy is also known as "happy eyeballs".
    disableIpv6ForBackup = true;
  }

  if (mEnt->mConnInfo->HasIPHintAddress()) {
    nsresult rv;
    nsCOMPtr<nsIDNSService> dns =
        do_GetService("@mozilla.org/network/dns-service;1", &rv);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // The spec says: "If A and AAAA records for TargetName are locally
    // available, the client SHOULD ignore these hints.", so we check if the DNS
    // record is in cache before setting USE_IP_HINT_ADDRESS.
    nsCOMPtr<nsIDNSRecord> record;
    rv = dns->ResolveNative(
        mPrimaryTransport.mHost, nsIDNSService::RESOLVE_OFFLINE,
        mEnt->mConnInfo->GetOriginAttributes(), getter_AddRefs(record));
    if (NS_FAILED(rv) || !record) {
      LOG(("Setting Socket to use IP hint address"));
      dnsFlags |= nsIDNSService::RESOLVE_IP_HINT;
    }
  }

  dnsFlags |=
      nsIDNSService::GetFlagsFromTRRMode(NS_HTTP_TRR_MODE_FROM_FLAGS(mCaps));

  // When we get here, we are not resolving using any configured proxy likely
  // because of individual proxy setting on the request or because the host is
  // excluded from proxying.  Hence, force resolution despite global proxy-DNS
  // configuration.
  dnsFlags |= nsIDNSService::RESOLVE_IGNORE_SOCKS_DNS;

  NS_ASSERTION(!(dnsFlags & nsIDNSService::RESOLVE_DISABLE_IPV6) ||
                   !(dnsFlags & nsIDNSService::RESOLVE_DISABLE_IPV4),
               "Setting both RESOLVE_DISABLE_IPV6 and RESOLVE_DISABLE_IPV4");

  mPrimaryTransport.mDnsFlags = dnsFlags;
  mBackupTransport.mDnsFlags = dnsFlags;
  if (disableIpv6ForBackup) {
    mBackupTransport.mDnsFlags |= nsISocketTransport::DISABLE_IPV6;
  }
  NS_ASSERTION(
      !( mBackupTransport.mDnsFlags & nsIDNSService::RESOLVE_DISABLE_IPV6) ||
           !( mBackupTransport.mDnsFlags & nsIDNSService::RESOLVE_DISABLE_IPV4),
      "Setting both RESOLVE_DISABLE_IPV6 and RESOLVE_DISABLE_IPV4");
  return NS_OK;
}

nsresult DnsAndConnectSocket::SetupEvent(SetupEvents event) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  LOG(("DnsAndConnectSocket::SetupEvent state=%d event=%d", mState, event));
  switch (event) {
    case SetupEvents::INIT_EVENT: {
      MOZ_ASSERT(mState == DnsAndSocketState::INIT);
      nsresult rv = mPrimaryTransport.Init(this);
      if (NS_FAILED(rv)) {
        mState = DnsAndSocketState::DONE;
        return rv;
      }
      if (mPrimaryTransport.FirstResolving()) {
        mState = DnsAndSocketState::RESOLVING;
      } else if (mPrimaryTransport.ConnectingOrRetry()) {
        mState = DnsAndSocketState::CONNECTING;
        SetupBackupTimer();
      } else {
        MOZ_ASSERT(false);
        return NS_ERROR_UNEXPECTED;
      }
    } break;
    case SetupEvents::RESOLVED_PRIMARY_EVENT:
      // This eventt may be posted multiple times if a DNS lookup is
      // retriggered, e.g with different parameter.
      if (mState == DnsAndSocketState::RESOLVING) {
        mState = DnsAndSocketState::CONNECTING;
        SetupBackupTimer();
      }
      break;
    case SetupEvents::PRIMARY_DONE_EVENT:
      MOZ_ASSERT((mState == DnsAndSocketState::RESOLVING) ||
                 (mState == DnsAndSocketState::CONNECTING) ||
                 (mState == DnsAndSocketState::ONE_CONNECTED));
      CancelBackupTimer();
      mBackupTransport.CancelDnsResolution();
      if (mBackupTransport.ConnectingOrRetry()) {
        mState = DnsAndSocketState::ONE_CONNECTED;
      } else {
        mState = DnsAndSocketState::DONE;
      }
      break;
    case SetupEvents::BACKUP_DONE_EVENT:
      MOZ_ASSERT((mState == DnsAndSocketState::CONNECTING) ||
                 (mState == DnsAndSocketState::ONE_CONNECTED));
      if (mPrimaryTransport.ConnectingOrRetry()) {
        mState = DnsAndSocketState::ONE_CONNECTED;
      } else {
        mState = DnsAndSocketState::DONE;
      }
      break;
    case SetupEvents::BACKUP_TIMER_FIRED_EVENT:
      MOZ_ASSERT(mState == DnsAndSocketState::CONNECTING);
      mBackupTransport.Init(this);
  }
  LOG(("DnsAndConnectSocket::SetupEvent state=%d", mState));

  return NS_OK;
}

void DnsAndConnectSocket::SetupBackupTimer() {
  MOZ_ASSERT(mEnt);
  uint16_t timeout = gHttpHandler->GetIdleSynTimeout();
  MOZ_ASSERT(!mSynTimer, "timer already initd");

  // When using Fast Open the correct transport will be setup for sure (it is
  // guaranteed), but it can be that it will happened a bit later.
  if (timeout && !mSpeculative && !mIsHttp3) {
    // Setup the timer that will establish a backup socket
    // if we do not get a writable event on the main one.
    // We do this because a lost SYN takes a very long time
    // to repair at the TCP level.
    //
    // Failure to setup the timer is something we can live with,
    // so don't return an error in that case.
    NS_NewTimerWithCallback(getter_AddRefs(mSynTimer), this, timeout,
                            nsITimer::TYPE_ONE_SHOT);
    LOG(("DnsAndConnectSocket::SetupBackupTimer() [this=%p]", this));
  } else if (timeout) {
    LOG(("DnsAndConnectSocket::SetupBackupTimer() [this=%p], did not arm\n",
         this));
  }
}

void DnsAndConnectSocket::CancelBackupTimer() {
  // If the syntimer is still armed, we can cancel it because no backup
  // socket should be formed at this point
  if (!mSynTimer) {
    return;
  }

  LOG(("DnsAndConnectSocket::CancelBackupTimer()"));
  mSynTimer->Cancel();

  // Keeping the reference to the timer to remember we have already
  // performed the backup connection.
}

void DnsAndConnectSocket::Abandon() {
  LOG(("DnsAndConnectSocket::Abandon [this=%p ent=%s] %p %p %p %p", this,
       mEnt->mConnInfo->Origin(), mPrimaryTransport.mSocketTransport.get(),
       mBackupTransport.mSocketTransport.get(),
       mPrimaryTransport.mStreamOut.get(), mBackupTransport.mStreamOut.get()));

  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  RefPtr<DnsAndConnectSocket> deleteProtector(this);

  // Tell socket (and backup socket) to forget the half open socket.
  mPrimaryTransport.Abandon();
  mBackupTransport.Abandon();

  // Stop the timer - we don't want any new backups.
  CancelBackupTimer();

  mState = DnsAndSocketState::DONE;

  // Remove the half open from the connection entry.
  if (mEnt) {
    mEnt->mDoNotDestroy = false;
    mEnt->RemoveDnsAndConnectSocket(this);
  }
  mEnt = nullptr;
}

double DnsAndConnectSocket::Duration(TimeStamp epoch) {
  if (mPrimaryTransport.mSynStarted.IsNull()) {
    return 0;
  }

  return (epoch - mPrimaryTransport.mSynStarted).ToMilliseconds();
}

NS_IMETHODIMP  // method for nsITimerCallback
DnsAndConnectSocket::Notify(nsITimer* timer) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  MOZ_ASSERT(timer == mSynTimer, "wrong timer");

  MOZ_ASSERT(!mBackupTransport.mDNSRequest);
  MOZ_ASSERT(!mBackupTransport.mSocketTransport);
  MOZ_ASSERT(mSynTimer);
  MOZ_ASSERT(mEnt);

  DebugOnly<nsresult> rv = SetupEvent(BACKUP_TIMER_FIRED_EVENT);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  // Keeping the reference to the timer to remember we have already
  // performed the backup connection.

  return NS_OK;
}

NS_IMETHODIMP  // method for nsINamed
DnsAndConnectSocket::GetName(nsACString& aName) {
  aName.AssignLiteral("DnsAndConnectSocket");
  return NS_OK;
}

NS_IMETHODIMP
DnsAndConnectSocket::OnLookupComplete(nsICancelable* request, nsIDNSRecord* rec,
                                      nsresult status) {
  LOG(("DnsAndConnectSocket::OnLookupComplete: this=%p status %" PRIx32 ".",
       this, static_cast<uint32_t>(status)));

  RefPtr<DnsAndConnectSocket> deleteProtector(this);

  if (!IsPrimary(request) && !IsBackup(request)) {
    return NS_OK;
  }

  if (IsPrimary(request) && NS_SUCCEEDED(status)) {
    mTransaction->OnTransportStatus(nullptr, NS_NET_STATUS_RESOLVED_HOST, 0);
  }

  // When using a HTTP proxy, NS_ERROR_UNKNOWN_HOST means the HTTP
  // proxy host is not found, so we fixup the error code.
  // For SOCKS proxies (mProxyTransparent == true), the socket
  // transport resolves the real host here, so there's no fixup
  // (see bug 226943).
  if (mProxyNotTransparent && (status == NS_ERROR_UNKNOWN_HOST)) {
    status = NS_ERROR_UNKNOWN_PROXY_HOST;
  }

  nsresult rv;
  // remember if it was primary because TransportSetups will delete the ref to
  // the DNS request and check cannot be performed later.
  bool isPrimary = IsPrimary(request);
  if (IsPrimary(request)) {
    rv = mPrimaryTransport.OnLookupComplete(this, rec, status);
    if (mPrimaryTransport.ConnectingOrRetry()) {
      SetupEvent(SetupEvents::RESOLVED_PRIMARY_EVENT);
    }
  } else {
    rv = mBackupTransport.OnLookupComplete(this, rec, status);
  }

  if (NS_FAILED(rv)) {
    SetupConn(isPrimary, rv);
  }
  return NS_OK;
}

// method for nsIAsyncOutputStreamCallback
NS_IMETHODIMP
DnsAndConnectSocket::OnOutputStreamReady(nsIAsyncOutputStream* out) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  MOZ_ASSERT(mPrimaryTransport.mSocketTransport ||
             mBackupTransport.mSocketTransport);
  MOZ_ASSERT(IsPrimary(out) || IsBackup(out), "stream mismatch");
  MOZ_ASSERT(mEnt);

  RefPtr<DnsAndConnectSocket> deleteProtector(this);

  LOG(("DnsAndConnectSocket::OnOutputStreamReady [this=%p ent=%s %s]\n", this,
       mEnt->mConnInfo->Origin(), IsPrimary(out) ? "primary" : "backup"));

  // Remember if it was primary or backup reuest.
  bool isPrimary = IsPrimary(out);
  nsresult rv = NS_OK;
  if (isPrimary) {
    rv = mPrimaryTransport.CheckConnectedResult(this);
    if (!mPrimaryTransport.DoneConnecting()) {
      return NS_OK;
    }
    MOZ_ASSERT((NS_SUCCEEDED(rv) &&
                (mPrimaryTransport.mState ==
                 TransportSetup::TransportSetupState::CONNECTING_DONE) &&
                mPrimaryTransport.mSocketTransport) ||
               (NS_FAILED(rv) &&
                (mPrimaryTransport.mState ==
                 TransportSetup::TransportSetupState::DONE) &&
                !mPrimaryTransport.mSocketTransport));
  } else if (IsBackup(out)) {
    rv = mBackupTransport.CheckConnectedResult(this);
    if (!mBackupTransport.DoneConnecting()) {
      return NS_OK;
    }
    MOZ_ASSERT((NS_SUCCEEDED(rv) &&
                (mBackupTransport.mState ==
                 TransportSetup::TransportSetupState::CONNECTING_DONE) &&
                mBackupTransport.mSocketTransport) ||
               (NS_FAILED(rv) &&
                (mBackupTransport.mState ==
                 TransportSetup::TransportSetupState::DONE) &&
                !mBackupTransport.mSocketTransport));
  } else {
    MOZ_ASSERT(false, "unexpected stream");
    return NS_ERROR_UNEXPECTED;
  }

  mEnt->mDoNotDestroy = true;
  gHttpHandler->ConnMgr()->RecvdConnect();

  rv = SetupConn(isPrimary, rv);
  if (mEnt) {
    mEnt->mDoNotDestroy = false;
  }
  return rv;
}

nsresult DnsAndConnectSocket::SetupConn(bool isPrimary, nsresult status) {
  // assign the new socket to the http connection
  RefPtr<HttpConnectionBase> conn;

  nsresult rv = NS_OK;
  if (isPrimary) {
    SetupEvent(SetupEvents::PRIMARY_DONE_EVENT);
    rv = mPrimaryTransport.SetupConn(mTransaction, mEnt, status,
                                     getter_AddRefs(conn));
  } else {
    SetupEvent(SetupEvents::BACKUP_DONE_EVENT);
    rv = mBackupTransport.SetupConn(mTransaction, mEnt, status,
                                    getter_AddRefs(conn));
  }

  nsCOMPtr<nsIInterfaceRequestor> callbacks;
  mTransaction->GetSecurityCallbacks(getter_AddRefs(callbacks));

  if (NS_FAILED(rv)) {
    LOG(
        ("DnsAndConnectSocket::SetupConn "
         "conn->init (%p) failed %" PRIx32 "\n",
         conn.get(), static_cast<uint32_t>(rv)));

    if (nsHttpTransaction* trans = mTransaction->QueryHttpTransaction()) {
      if (mIsHttp3) {
        trans->DisableHttp3();
        gHttpHandler->ExcludeHttp3(mEnt->mConnInfo);
      }
      // The transaction's connection info is changed after DisableHttp3(), so
      // this is the only point we can remove this transaction from its conn
      // entry.
      mEnt->RemoveTransFromPendingQ(trans);
    }
    mTransaction->Close(rv);

    return rv;
  }

  // This half-open socket has created a connection.  This flag excludes it
  // from counter of actual connections used for checking limits.
  mHasConnected = true;

  // if this is still in the pending list, remove it and dispatch it
  RefPtr<PendingTransactionInfo> pendingTransInfo =
      gHttpHandler->ConnMgr()->FindTransactionHelper(true, mEnt, mTransaction);
  if (pendingTransInfo) {
    MOZ_ASSERT(!mSpeculative, "Speculative Half Open found mTransaction");

    mEnt->InsertIntoActiveConns(conn);
    if (mIsHttp3) {
      // Each connection must have a ConnectionHandle wrapper.
      // In case of Http < 2 the a ConnectionHandle is created for each
      // transaction in DispatchAbstractTransaction.
      // In case of Http2/ and Http3, ConnectionHandle is created only once.
      // Http2 connection always starts as http1 connection and the first
      // transaction use DispatchAbstractTransaction to be dispatched and
      // a ConnectionHandle is created. All consecutive transactions for
      // Http2 use a short-cut in DispatchTransaction and call
      // HttpConnectionBase::Activate (DispatchAbstractTransaction is never
      // called).
      // In case of Http3 the short-cut HttpConnectionBase::Activate is always
      // used also for the first transaction, therefore we need to create
      // ConnectionHandle here.
      RefPtr<ConnectionHandle> handle = new ConnectionHandle(conn);
      pendingTransInfo->Transaction()->SetConnection(handle);
    }
    rv = gHttpHandler->ConnMgr()->DispatchTransaction(
        mEnt, pendingTransInfo->Transaction(), conn);
  } else {
    // this transaction was dispatched off the pending q before all the
    // sockets established themselves.

    // After about 1 second allow for the possibility of restarting a
    // transaction due to server close. Keep at sub 1 second as that is the
    // minimum granularity we can expect a server to be timing out with.
    RefPtr<nsHttpConnection> connTCP = do_QueryObject(conn);
    if (connTCP) {
      connTCP->SetIsReusedAfter(950);
    }

    // if we are using ssl and no other transactions are waiting right now,
    // then form a null transaction to drive the SSL handshake to
    // completion. Afterwards the connection will be 100% ready for the next
    // transaction to use it. Make an exception for SSL tunneled HTTP proxy as
    // the NullHttpTransaction does not know how to drive Connect
    // Http3 cannot be dispatched using OnMsgReclaimConnection (see below),
    // therefore we need to use a Nulltransaction.
    if (!connTCP ||
        (mEnt->mConnInfo->FirstHopSSL() && !mEnt->UrgentStartQueueLength() &&
         !mEnt->PendingQueueLength() && !mEnt->mConnInfo->UsingConnect())) {
      LOG(
          ("DnsAndConnectSocket::SetupConn null transaction will "
           "be used to finish SSL handshake on conn %p\n",
           conn.get()));
      RefPtr<nsAHttpTransaction> trans;
      if (mTransaction->IsNullTransaction() && !mDispatchedMTransaction) {
        // null transactions cannot be put in the entry queue, so that
        // explains why it is not present.
        mDispatchedMTransaction = true;
        trans = mTransaction;
      } else {
        trans = new NullHttpTransaction(mEnt->mConnInfo, callbacks, mCaps);
      }

      mEnt->InsertIntoActiveConns(conn);
      rv = gHttpHandler->ConnMgr()->DispatchAbstractTransaction(mEnt, trans,
                                                                mCaps, conn, 0);
    } else {
      // otherwise just put this in the persistent connection pool
      LOG(
          ("DnsAndConnectSocket::SetupConn no transaction match "
           "returning conn %p to pool\n",
           conn.get()));
      gHttpHandler->ConnMgr()->OnMsgReclaimConnection(conn);

      // We expect that there is at least one tranasction in the pending
      // queue that can take this connection, but it can happened that
      // all transactions are blocked or they have took other idle
      // connections. In that case the connection has been added to the
      // idle queue.
      // If the connection is in the idle queue but it is using ssl, make
      // a nulltransaction for it to finish ssl handshake!

      // !!! It can be that mEnt is null after OnMsgReclaimConnection.!!!
      if (mEnt && mEnt->mConnInfo->FirstHopSSL() &&
          !mEnt->mConnInfo->UsingConnect()) {
        RefPtr<nsHttpConnection> connTCP = do_QueryObject(conn);
        // If RemoveIdleConnection succeeds that means that conn is in the
        // idle queue.
        if (connTCP && NS_SUCCEEDED(mEnt->RemoveIdleConnection(connTCP))) {
          RefPtr<nsAHttpTransaction> trans;
          if (mTransaction->IsNullTransaction() && !mDispatchedMTransaction) {
            mDispatchedMTransaction = true;
            trans = mTransaction;
          } else {
            trans = new NullHttpTransaction(mEnt->mConnInfo, callbacks, mCaps);
          }
          mEnt->InsertIntoActiveConns(conn);
          rv = gHttpHandler->ConnMgr()->DispatchAbstractTransaction(
              mEnt, trans, mCaps, conn, 0);
        }
      }
    }
  }

  // If this halfOpenConn was speculative, but at the end the conn got a
  // non-null transaction than this halfOpen is not speculative anymore!
  if (conn->Transaction() && !conn->Transaction()->IsNullTransaction()) {
    Claim();
  }

  return rv;
}

// method for nsITransportEventSink
NS_IMETHODIMP
DnsAndConnectSocket::OnTransportStatus(nsITransport* trans, nsresult status,
                                       int64_t progress, int64_t progressMax) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  MOZ_ASSERT(IsPrimary(trans) || IsBackup(trans));
  MOZ_ASSERT(mEnt);
  if (mTransaction) {
    if (IsPrimary(trans) ||
        (IsBackup(trans) && (status == NS_NET_STATUS_CONNECTED_TO) &&
         mPrimaryTransport.mSocketTransport)) {
      // Send this status event only if the transaction is still pending,
      // i.e. it has not found a free already connected socket.
      // Sockets in halfOpen state can only get following events:
      // NS_NET_STATUS_CONNECTING_TO and NS_NET_STATUS_CONNECTED_TO.
      // mBackupTransport is only started after
      // NS_NET_STATUS_CONNECTING_TO of mSocketTransport, so ignore
      // NS_NET_STATUS_CONNECTING_TO event for mBackupTransport and
      // send NS_NET_STATUS_CONNECTED_TO.
      // mBackupTransport must be connected before mSocketTransport(e.g.
      // mPrimaryTransport.mSocketTransport != nullpttr).
      mTransaction->OnTransportStatus(trans, status, progress);
    }
  }

  if (status == NS_NET_STATUS_CONNECTED_TO) {
    if (IsPrimary(trans)) {
      mPrimaryTransport.mConnectedOK = true;
    } else {
      mBackupTransport.mConnectedOK = true;
    }
  }

  // The rest of this method only applies to the primary transport
  if (!IsPrimary(trans)) {
    return NS_OK;
  }

  // if we are doing spdy coalescing and haven't recorded the ip address
  // for this entry before then make the hash key if our dns lookup
  // just completed. We can't do coalescing if using a proxy because the
  // ip addresses are not available to the client.

  if (status == NS_NET_STATUS_CONNECTING_TO && gHttpHandler->IsSpdyEnabled() &&
      gHttpHandler->CoalesceSpdy() && mEnt && mEnt->mConnInfo &&
      mEnt->mConnInfo->EndToEndSSL() && mEnt->AllowHttp2() &&
      !mEnt->mConnInfo->UsingProxy() && mEnt->mCoalescingKeys.IsEmpty()) {
    nsCOMPtr<nsIDNSAddrRecord> dnsRecord(
        do_GetInterface(mPrimaryTransport.mSocketTransport));
    nsTArray<NetAddr> addressSet;
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    if (dnsRecord) {
      rv = dnsRecord->GetAddresses(addressSet);
    }

    if (NS_SUCCEEDED(rv) && !addressSet.IsEmpty()) {
      for (uint32_t i = 0; i < addressSet.Length(); ++i) {
        if ((addressSet[i].raw.family == AF_INET &&
             addressSet[i].inet.ip == 0) ||
            (addressSet[i].raw.family == AF_INET6 &&
             addressSet[i].inet6.ip.u64[0] == 0 &&
             addressSet[i].inet6.ip.u64[1] == 0)) {
          // Bug 1680249 - Don't create the coalescing key if the ip address is
          // `0.0.0.0` or `::`.
          LOG((
              "DnsAndConnectSocket: skip creating Coalescing Key for host [%s]",
              mEnt->mConnInfo->Origin()));
          continue;
        }
        nsCString* newKey = mEnt->mCoalescingKeys.AppendElement(nsCString());
        newKey->SetLength(kIPv6CStrBufSize + 26);
        addressSet[i].ToStringBuffer(newKey->BeginWriting(), kIPv6CStrBufSize);
        newKey->SetLength(strlen(newKey->BeginReading()));
        if (mEnt->mConnInfo->GetAnonymous()) {
          newKey->AppendLiteral("~A:");
        } else {
          newKey->AppendLiteral("~.:");
        }
        newKey->AppendInt(mEnt->mConnInfo->OriginPort());
        newKey->AppendLiteral("/[");
        nsAutoCString suffix;
        mEnt->mConnInfo->GetOriginAttributes().CreateSuffix(suffix);
        newKey->Append(suffix);
        newKey->AppendLiteral("]viaDNS");
        LOG((
            "DnsAndConnectSocket::OnTransportStatus "
            "STATUS_CONNECTING_TO Established New Coalescing Key # %d for host "
            "%s [%s]",
            i, mEnt->mConnInfo->Origin(), newKey->get()));
      }
      gHttpHandler->ConnMgr()->ProcessSpdyPendingQ(mEnt);
    }
  }

  return NS_OK;
}

bool DnsAndConnectSocket::IsPrimary(nsITransport* trans) {
  return trans == mPrimaryTransport.mSocketTransport;
}

bool DnsAndConnectSocket::IsPrimary(nsIAsyncOutputStream* out) {
  return out == mPrimaryTransport.mStreamOut;
}

bool DnsAndConnectSocket::IsPrimary(nsICancelable* dnsRequest) {
  return dnsRequest == mPrimaryTransport.mDNSRequest;
}

bool DnsAndConnectSocket::IsBackup(nsITransport* trans) {
  return trans == mBackupTransport.mSocketTransport;
}

bool DnsAndConnectSocket::IsBackup(nsIAsyncOutputStream* out) {
  return out == mBackupTransport.mStreamOut;
}

bool DnsAndConnectSocket::IsBackup(nsICancelable* dnsRequest) {
  return dnsRequest == mBackupTransport.mDNSRequest;
}

// method for nsIInterfaceRequestor
NS_IMETHODIMP
DnsAndConnectSocket::GetInterface(const nsIID& iid, void** result) {
  if (mTransaction) {
    nsCOMPtr<nsIInterfaceRequestor> callbacks;
    mTransaction->GetSecurityCallbacks(getter_AddRefs(callbacks));
    if (callbacks) {
      return callbacks->GetInterface(iid, result);
    }
  }
  return NS_ERROR_NO_INTERFACE;
}

bool DnsAndConnectSocket::AcceptsTransaction(nsHttpTransaction* trans) {
  // When marked as urgent start, only accept urgent start marked transactions.
  // Otherwise, accept any kind of transaction.
  return !mUrgentStart || (trans->Caps() & nsIClassOfService::UrgentStart);
}

bool DnsAndConnectSocket::Claim() {
  if (mSpeculative) {
    mSpeculative = false;
    uint32_t flags;
    if (mPrimaryTransport.mSocketTransport &&
        NS_SUCCEEDED(
            mPrimaryTransport.mSocketTransport->GetConnectionFlags(&flags))) {
      flags &= ~nsISocketTransport::DISABLE_RFC1918;
      mPrimaryTransport.mSocketTransport->SetConnectionFlags(flags);
    }

    Telemetry::AutoCounter<Telemetry::HTTPCONNMGR_USED_SPECULATIVE_CONN>
        usedSpeculativeConn;
    ++usedSpeculativeConn;

    if (mIsFromPredictor) {
      Telemetry::AutoCounter<Telemetry::PREDICTOR_TOTAL_PRECONNECTS_USED>
          totalPreconnectsUsed;
      ++totalPreconnectsUsed;
    }

    // Http3 has its own syn-retransmission, therefore it does not need a
    // backup connection.
    if (mPrimaryTransport.ConnectingOrRetry() && mEnt &&
        !mBackupTransport.mSocketTransport && !mSynTimer && !mIsHttp3) {
      SetupBackupTimer();
    }
  }

  if (mFreeToUse) {
    mFreeToUse = false;
    return true;
  }
  return false;
}

void DnsAndConnectSocket::Unclaim() {
  MOZ_ASSERT(!mSpeculative && !mFreeToUse);
  // We will keep the backup-timer running. Most probably this halfOpen will
  // be used by a transaction from which this transaction took the halfOpen.
  // (this is happening because of the transaction priority.)
  mFreeToUse = true;
}

void DnsAndConnectSocket::CloseTransports(nsresult error) {
  if (mPrimaryTransport.mSocketTransport) {
    mPrimaryTransport.mSocketTransport->Close(error);
  }
  if (mBackupTransport.mSocketTransport) {
    mBackupTransport.mSocketTransport->Close(error);
  }
}

DnsAndConnectSocket::TransportSetup::TransportSetup(bool isBackup)
    : mState(TransportSetup::TransportSetupState::INIT), mIsBackup(isBackup) {}

nsresult DnsAndConnectSocket::TransportSetup::Init(
    DnsAndConnectSocket* dnsAndSock) {
  nsresult rv;
  mSynStarted = TimeStamp::Now();
  if (mSkipDnsResolution) {
    mState = TransportSetup::TransportSetupState::CONNECTING;
    rv = SetupStreams(dnsAndSock);
  } else {
    mState = TransportSetup::TransportSetupState::RESOLVING;
    rv = ResolveHost(dnsAndSock);
  }
  if (NS_FAILED(rv)) {
    CloseAll();
    mState = TransportSetup::TransportSetupState::DONE;
  }
  return rv;
}

void DnsAndConnectSocket::TransportSetup::CancelDnsResolution() {
  if (mDNSRequest) {
    mDNSRequest->Cancel(NS_ERROR_ABORT);
    mDNSRequest = nullptr;
  }
  if (mState == TransportSetup::TransportSetupState::RESOLVING) {
    mState = TransportSetup::TransportSetupState::INIT;
  }
}

void DnsAndConnectSocket::TransportSetup::Abandon() {
  CloseAll();
  mState = TransportSetup::TransportSetupState::DONE;
}

void DnsAndConnectSocket::TransportSetup::CloseAll() {
  // Tell socket (and backup socket) to forget the half open socket.
  if (mSocketTransport) {
    mSocketTransport->SetEventSink(nullptr, nullptr);
    mSocketTransport->SetSecurityCallbacks(nullptr);
    mSocketTransport = nullptr;
  }

  // Tell output stream (and backup) to forget the half open socket.
  if (mStreamOut) {
    gHttpHandler->ConnMgr()->RecvdConnect();
    mStreamOut->AsyncWait(nullptr, 0, 0, nullptr);
    mStreamOut = nullptr;
  }

  // Lose references to input stream (and backup).
  if (mStreamIn) {
    mStreamIn->AsyncWait(nullptr, 0, 0, nullptr);
    mStreamIn = nullptr;
  }

  if (mDNSRequest) {
    mDNSRequest->Cancel(NS_ERROR_ABORT);
    mDNSRequest = nullptr;
  }

  mConnectedOK = false;
}

nsresult DnsAndConnectSocket::TransportSetup::CheckConnectedResult(
    DnsAndConnectSocket* dnsAndSock) {
  mState = TransportSetup::TransportSetupState::CONNECTING_DONE;

  if (mSkipDnsResolution) {
    return NS_OK;
  }
  bool retryDns = false;
  mSocketTransport->GetRetryDnsIfPossible(&retryDns);
  if (!retryDns) {
    return NS_OK;
  }

  bool retry = false;
  if (mRetryWithDifferentIPFamily) {
    mRetryWithDifferentIPFamily = false;
    mDnsFlags ^= (nsIDNSService::RESOLVE_DISABLE_IPV6 |
                  nsIDNSService::RESOLVE_DISABLE_IPV4);
    mResetFamilyPreference = true;
    retry = true;
  } else if (!(mDnsFlags & nsIDNSService::RESOLVE_DISABLE_TRR)) {
    bool trrEnabled;
    mDNSRecord->IsTRR(&trrEnabled);
    if (trrEnabled) {
      uint32_t trrMode = 0;
      mDNSRecord->GetEffectiveTRRMode(&trrMode);
      // If current trr mode is trr only, we should not retry.
      if (trrMode != 3) {
        // Drop state to closed.  This will trigger a new round of
        // DNS resolving. Bypass the cache this time since the
        // cached data came from TRR and failed already!
        LOG(("  failed to connect with TRR enabled, try w/o\n"));
        mDnsFlags |= nsIDNSService::RESOLVE_DISABLE_TRR |
                     nsIDNSService::RESOLVE_BYPASS_CACHE |
                     nsIDNSService::RESOLVE_REFRESH_CACHE;
        retry = true;
      }
    }
  }

  if (retry) {
    CloseAll();
    mState = TransportSetup::TransportSetupState::RESOLVING;
    nsresult rv = ResolveHost(dnsAndSock);
    if (NS_FAILED(rv)) {
      CloseAll();
      mState = TransportSetup::TransportSetupState::DONE;
    }
    return rv;
  }

  return NS_OK;
}

nsresult DnsAndConnectSocket::TransportSetup::SetupConn(
    nsAHttpTransaction* transaction, ConnectionEntry* ent, nsresult status,
    HttpConnectionBase** connection) {
  RefPtr<HttpConnectionBase> conn;
  if (!ent->mConnInfo->IsHttp3()) {
    conn = new nsHttpConnection();
  } else {
    conn = new HttpConnectionUDP();
  }

  LOG(
      ("DnsAndConnectSocket::SocketTransport::SetupConn "
       "Created new nshttpconnection %p %s\n",
       conn.get(), ent->mConnInfo->IsHttp3() ? "using http3" : ""));

  NullHttpTransaction* nullTrans = transaction->QueryNullTransaction();
  if (nullTrans) {
    conn->BootstrapTimings(nullTrans->Timings());
  }

  // Some capabilities are needed before a transaction actually gets
  // scheduled (e.g. how to negotiate false start)
  conn->SetTransactionCaps(transaction->Caps());

  nsCOMPtr<nsIInterfaceRequestor> callbacks;
  transaction->GetSecurityCallbacks(getter_AddRefs(callbacks));
  nsresult rv = conn->Init(
      ent->mConnInfo, gHttpHandler->ConnMgr()->mMaxRequestDelay,
      mSocketTransport, mStreamIn, mStreamOut, mConnectedOK, status, callbacks,
      PR_MillisecondsToInterval(static_cast<uint32_t>(
          (TimeStamp::Now() - mSynStarted).ToMilliseconds())));

  bool resetPreference = false;
  if (mResetFamilyPreference ||
      (mSocketTransport &&
       NS_SUCCEEDED(
           mSocketTransport->GetResetIPFamilyPreference(&resetPreference)) &&
       resetPreference)) {
    ent->ResetIPFamilyPreference();
  }

  NetAddr peeraddr;
  if (mSocketTransport &&
      NS_SUCCEEDED(mSocketTransport->GetPeerAddr(&peeraddr))) {
    ent->RecordIPFamilyPreference(peeraddr.raw.family);
  }
  conn.forget(connection);
  mSocketTransport = nullptr;
  mStreamOut = nullptr;
  mStreamIn = nullptr;
  mState = TransportSetup::TransportSetupState::DONE;
  return rv;
}

nsresult DnsAndConnectSocket::TransportSetup::SetupStreams(
    DnsAndConnectSocket* dnsAndSock) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  MOZ_ASSERT(dnsAndSock->mEnt);
  nsresult rv;
  nsTArray<nsCString> socketTypes;
  const nsHttpConnectionInfo* ci = dnsAndSock->mEnt->mConnInfo;
  if (dnsAndSock->mIsHttp3) {
    socketTypes.AppendElement("quic"_ns);
  } else {
    if (ci->FirstHopSSL()) {
      socketTypes.AppendElement("ssl"_ns);
    } else {
      const nsCString& defaultType = gHttpHandler->DefaultSocketType();
      if (!defaultType.IsVoid()) {
        socketTypes.AppendElement(defaultType);
      }
    }
  }

  nsCOMPtr<nsISocketTransport> socketTransport;
  nsCOMPtr<nsISocketTransportService> sts;

  sts = components::SocketTransport::Service();
  if (!sts) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  LOG(
      ("DnsAndConnectSocket::SetupStreams [this=%p ent=%s] "
       "setup routed transport to origin %s:%d via %s:%d\n",
       this, ci->HashKey().get(), ci->Origin(), ci->OriginPort(),
       ci->RoutedHost(), ci->RoutedPort()));

  nsCOMPtr<nsIRoutedSocketTransportService> routedSTS(do_QueryInterface(sts));
  if (routedSTS) {
    rv = routedSTS->CreateRoutedTransport(
        socketTypes, ci->GetOrigin(), ci->OriginPort(), ci->GetRoutedHost(),
        ci->RoutedPort(), ci->ProxyInfo(), mDNSRecord,
        getter_AddRefs(socketTransport));
  } else {
    if (!ci->GetRoutedHost().IsEmpty()) {
      // There is a route requested, but the legacy nsISocketTransportService
      // can't handle it.
      // Origin should be reachable on origin host name, so this should
      // not be a problem - but log it.
      LOG(
          ("DnsAndConnectSocket this=%p using legacy nsISocketTransportService "
           "means explicit route %s:%d will be ignored.\n",
           this, ci->RoutedHost(), ci->RoutedPort()));
    }

    rv = sts->CreateTransport(socketTypes, ci->GetOrigin(), ci->OriginPort(),
                              ci->ProxyInfo(), mDNSRecord,
                              getter_AddRefs(socketTransport));
  }
  NS_ENSURE_SUCCESS(rv, rv);

  uint32_t tmpFlags = 0;
  if (dnsAndSock->mCaps & NS_HTTP_REFRESH_DNS) {
    tmpFlags = nsISocketTransport::BYPASS_CACHE;
  }

  tmpFlags |= nsISocketTransport::GetFlagsFromTRRMode(
      NS_HTTP_TRR_MODE_FROM_FLAGS(dnsAndSock->mCaps));

  if (dnsAndSock->mCaps & NS_HTTP_LOAD_ANONYMOUS) {
    tmpFlags |= nsISocketTransport::ANONYMOUS_CONNECT;
  }

  // When we are making a speculative connection we do not propagate all flags
  // in mCaps, so we need to query nsHttpConnectionInfo directly as well.
  if ((dnsAndSock->mCaps & NS_HTTP_LOAD_ANONYMOUS_CONNECT_ALLOW_CLIENT_CERT) ||
      ci->GetAnonymousAllowClientCert()) {
    tmpFlags |= nsISocketTransport::ANONYMOUS_CONNECT_ALLOW_CLIENT_CERT;
  }

  if (ci->GetPrivate()) {
    tmpFlags |= nsISocketTransport::NO_PERMANENT_STORAGE;
  }

  Unused << socketTransport->SetIsPrivate(ci->GetPrivate());

  if (ci->GetLessThanTls13()) {
    tmpFlags |= nsISocketTransport::DONT_TRY_ECH;
  }

  if (((dnsAndSock->mCaps & NS_HTTP_BE_CONSERVATIVE) ||
       ci->GetBeConservative()) &&
      gHttpHandler->ConnMgr()->BeConservativeIfProxied(ci->ProxyInfo())) {
    LOG(("Setting Socket to BE_CONSERVATIVE"));
    tmpFlags |= nsISocketTransport::BE_CONSERVATIVE;
  }

  if (ci->HasIPHintAddress()) {
    nsCOMPtr<nsIDNSService> dns =
        do_GetService("@mozilla.org/network/dns-service;1", &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // The spec says: "If A and AAAA records for TargetName are locally
    // available, the client SHOULD ignore these hints.", so we check if the DNS
    // record is in cache before setting USE_IP_HINT_ADDRESS.
    nsCOMPtr<nsIDNSRecord> record;
    rv = dns->ResolveNative(mHost, nsIDNSService::RESOLVE_OFFLINE,
                            dnsAndSock->mEnt->mConnInfo->GetOriginAttributes(),
                            getter_AddRefs(record));
    if (NS_FAILED(rv) || !record) {
      LOG(("Setting Socket to use IP hint address"));
      tmpFlags |= nsISocketTransport::USE_IP_HINT_ADDRESS;
    }
  }

  if (dnsAndSock->mCaps & NS_HTTP_DISABLE_IPV4) {
    tmpFlags |= nsISocketTransport::DISABLE_IPV4;
  } else if (dnsAndSock->mCaps & NS_HTTP_DISABLE_IPV6) {
    tmpFlags |= nsISocketTransport::DISABLE_IPV6;
  } else if (dnsAndSock->mEnt->PreferenceKnown()) {
    if (dnsAndSock->mEnt->mPreferIPv6) {
      tmpFlags |= nsISocketTransport::DISABLE_IPV4;
    } else if (dnsAndSock->mEnt->mPreferIPv4) {
      tmpFlags |= nsISocketTransport::DISABLE_IPV6;
    }

    // In case the host is no longer accessible via the preferred IP family,
    // try the opposite one and potentially restate the preference.
    tmpFlags |= nsISocketTransport::RETRY_WITH_DIFFERENT_IP_FAMILY;

    // From the same reason, let the backup socket fail faster to try the other
    // family.
    uint16_t fallbackTimeout =
        mIsBackup ? gHttpHandler->GetFallbackSynTimeout() : 0;
    if (fallbackTimeout) {
      socketTransport->SetTimeout(nsISocketTransport::TIMEOUT_CONNECT,
                                  fallbackTimeout);
    }
  } else if (mIsBackup && gHttpHandler->FastFallbackToIPv4()) {
    // For backup connections, we disable IPv6. That's because some users have
    // broken IPv6 connectivity (leading to very long timeouts), and disabling
    // IPv6 on the backup connection gives them a much better user experience
    // with dual-stack hosts, though they still pay the 250ms delay for each new
    // connection. This strategy is also known as "happy eyeballs".
    tmpFlags |= nsISocketTransport::DISABLE_IPV6;
  }

  if (!dnsAndSock->Allow1918()) {
    tmpFlags |= nsISocketTransport::DISABLE_RFC1918;
  }

  MOZ_ASSERT(!(tmpFlags & nsISocketTransport::DISABLE_IPV4) ||
                 !(tmpFlags & nsISocketTransport::DISABLE_IPV6),
             "Both types should not be disabled at the same time.");
  socketTransport->SetConnectionFlags(tmpFlags);
  socketTransport->SetTlsFlags(ci->GetTlsFlags());

  const OriginAttributes& originAttributes =
      dnsAndSock->mEnt->mConnInfo->GetOriginAttributes();
  if (originAttributes != OriginAttributes()) {
    socketTransport->SetOriginAttributes(originAttributes);
  }

  socketTransport->SetQoSBits(gHttpHandler->GetQoSBits());

  rv = socketTransport->SetEventSink(dnsAndSock, nullptr);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = socketTransport->SetSecurityCallbacks(dnsAndSock);
  NS_ENSURE_SUCCESS(rv, rv);

  if (gHttpHandler->EchConfigEnabled()) {
    rv = socketTransport->SetEchConfig(ci->GetEchConfig());
    NS_ENSURE_SUCCESS(rv, rv);
  }

  Telemetry::Accumulate(Telemetry::HTTP_CONNECTION_ENTRY_CACHE_HIT_1,
                        dnsAndSock->mEnt->mUsedForConnection);
  dnsAndSock->mEnt->mUsedForConnection = true;

  nsCOMPtr<nsIOutputStream> sout;
  rv = socketTransport->OpenOutputStream(nsITransport::OPEN_UNBUFFERED, 0, 0,
                                         getter_AddRefs(sout));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIInputStream> sin;
  rv = socketTransport->OpenInputStream(nsITransport::OPEN_UNBUFFERED, 0, 0,
                                        getter_AddRefs(sin));
  NS_ENSURE_SUCCESS(rv, rv);

  mSocketTransport = socketTransport.forget();
  mStreamIn = do_QueryInterface(sin);
  mStreamOut = do_QueryInterface(sout);

  rv = mStreamOut->AsyncWait(dnsAndSock, 0, 0, nullptr);
  if (NS_SUCCEEDED(rv)) {
    gHttpHandler->ConnMgr()->StartedConnect();
  }

  return rv;
}

nsresult DnsAndConnectSocket::TransportSetup::ResolveHost(
    DnsAndConnectSocket* dnsAndSock) {
  LOG(("DnsAndConnectSocket::TransportSetup::ResolveHost [this=%p %s%s]", this,
       PromiseFlatCString(mHost).get(),
       (mDnsFlags & nsIDNSService::RESOLVE_BYPASS_CACHE) ? " bypass cache"
                                                         : ""));
  nsCOMPtr<nsIDNSService> dns = nullptr;
  auto initTask = [&dns]() { dns = do_GetService(NS_DNSSERVICE_CID); };
  if (!NS_IsMainThread()) {
    // Forward to the main thread synchronously.
    RefPtr<nsIThread> mainThread = do_GetMainThread();
    if (!mainThread) {
      return NS_ERROR_FAILURE;
    }

    SyncRunnable::DispatchToThread(
        mainThread,
        new SyncRunnable(NS_NewRunnableFunction(
            "nsSocketTransport::ResolveHost->GetDNSService", initTask)));
  } else {
    initTask();
  }
  if (!dns) {
    return NS_ERROR_FAILURE;
  }

  if (!mIsBackup) {
    dnsAndSock->mTransaction->OnTransportStatus(
        nullptr, NS_NET_STATUS_RESOLVING_HOST, 0);
  }

  return dns->AsyncResolveNative(
      mHost, nsIDNSService::RESOLVE_TYPE_DEFAULT, mDnsFlags, nullptr,
      dnsAndSock, gSocketTransportService,
      dnsAndSock->mEnt->mConnInfo->GetOriginAttributes(),
      getter_AddRefs(mDNSRequest));
}

nsresult DnsAndConnectSocket::TransportSetup::OnLookupComplete(
    DnsAndConnectSocket* dnsAndSock, nsIDNSRecord* rec, nsresult status) {
  mDNSRequest = nullptr;
  if (NS_SUCCEEDED(status)) {
    mDNSRecord = do_QueryInterface(rec);
    MOZ_ASSERT(mDNSRecord);

    nsresult rv = SetupStreams(dnsAndSock);
    if (NS_SUCCEEDED(rv)) {
      mState = TransportSetup::TransportSetupState::CONNECTING;
    } else {
      CloseAll();
      mState = TransportSetup::TransportSetupState::DONE;
    }
    return rv;
  }

  // DNS lookup status failed

  bool retry = false;
  if (mDnsFlags & nsIDNSService::RESOLVE_IP_HINT) {
    mDnsFlags &= ~nsIDNSService::RESOLVE_IP_HINT;
    retry = true;
  } else if ((status == NS_ERROR_UNKNOWN_HOST) && mRetryWithDifferentIPFamily) {
    mRetryWithDifferentIPFamily = false;
    mDnsFlags ^= (nsIDNSService::RESOLVE_DISABLE_IPV6 |
                  nsIDNSService::RESOLVE_DISABLE_IPV4);
    mResetFamilyPreference = true;
    retry = true;
  }

  if (retry) {
    mState = TransportSetup::TransportSetupState::RETRY_RESOLVING;
    nsresult rv = ResolveHost(dnsAndSock);
    if (NS_FAILED(rv)) {
      CloseAll();
      mState = TransportSetup::TransportSetupState::DONE;
    }
    return rv;
  }

  mState = TransportSetup::TransportSetupState::DONE;
  return status;
}

}  // namespace net
}  // namespace mozilla
