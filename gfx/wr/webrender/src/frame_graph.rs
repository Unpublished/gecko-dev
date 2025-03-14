// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

use api::units::*;
use api::ImageFormat;
use crate::gpu_cache::GpuCache;
use crate::internal_types::{CacheTextureId, FastHashMap, FastHashSet};
use crate::render_backend::FrameId;
use crate::render_task_graph::{RenderTaskId};
use crate::render_task::{StaticRenderTaskSurface, RenderTaskLocation, RenderTask};
use crate::render_target::RenderTargetKind;
use crate::render_task::{RenderTaskData, RenderTaskKind};
use crate::render_task_graph::RenderTaskAllocation;
use crate::resource_cache::ResourceCache;
use crate::texture_pack::GuillotineAllocator;
use crate::prim_store::DeferredResolve;
use crate::image_source::resolve_image;
use crate::util::VecHelper;
use smallvec::SmallVec;
use std::mem;

/// According to apitrace, textures larger than 2048 break fast clear
/// optimizations on some intel drivers. We sometimes need to go larger, but
/// we try to avoid it.
const MAX_SHARED_SURFACE_SIZE: i32 = 2048;

/// If we ever need a larger texture than the ideal, we better round it up to a
/// reasonable number in order to have a bit of leeway in case the size of this
/// this target is changing each frame.
const TEXTURE_DIMENSION_MASK: i32 = 0xFF;

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug, Copy, Clone, Hash, Eq, PartialEq, PartialOrd, Ord)]
pub struct PassId(usize);

impl PassId {
    pub const MIN: PassId = PassId(0);
    pub const MAX: PassId = PassId(!0);
}

/// An internal representation of a dynamic surface that tasks can be
/// allocated into. Maintains some extra metadata about each surface
/// during the graph build.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
struct Surface {
    /// Whether this is a color or alpha render target
    kind: RenderTargetKind,
    /// Allocator for this surface texture
    allocator: GuillotineAllocator,
    /// We can only allocate into this for reuse if it's a shared surface
    is_shared: bool,
}

impl Surface {
    /// Allocate a rect within a shared surfce. Returns None if the
    /// format doesn't match, or allocation fails.
    fn alloc_rect(
        &mut self,
        size: DeviceIntSize,
        kind: RenderTargetKind,
        is_shared: bool,
    ) -> Option<DeviceIntPoint> {
        if self.kind == kind && self.is_shared == is_shared {
            self.allocator
                .allocate(&size)
                .map(|(_slice, origin)| origin)
        } else {
            None
        }
    }
}

/// A sub-pass can draw to either a dynamic (temporary render target) surface,
/// or a persistent surface (texture or picture cache).
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug)]
pub enum SubPassSurface {
    /// A temporary (intermediate) surface.
    Dynamic {
        /// The renderer texture id
        texture_id: CacheTextureId,
        /// Color / alpha render target
        target_kind: RenderTargetKind,
        /// The rectangle occupied by tasks in this surface. Used as a clear
        /// optimization on some GPUs.
        used_rect: DeviceIntRect,
    },
    Persistent {
        /// Reference to the texture or picture cache surface being drawn to.
        surface: StaticRenderTaskSurface,
    },
}

/// A subpass is a specific render target, and a list of tasks to draw to it.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct SubPass {
    /// The surface this subpass draws to
    pub surface: SubPassSurface,
    /// The tasks assigned to this subpass.
    pub task_ids: Vec<RenderTaskId>,
}

/// A pass expresses dependencies between tasks. Each pass consists of a number
/// of subpasses.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct Pass {
    /// The tasks assigned to this render pass
    pub task_ids: Vec<RenderTaskId>,
    /// The subpasses that make up this dependency pass
    pub sub_passes: Vec<SubPass>,
    /// A list of intermediate surfaces that can be invalidated after
    /// this pass completes.
    pub textures_to_invalidate: Vec<CacheTextureId>,
}

/// The FrameGraph is the immutable representation of the render task graph. It is
/// built by the FrameGraphBuilder, and is constructed once per frame.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct FrameGraph {
    /// List of tasks added to the graph
    pub tasks: Vec<RenderTask>,

    /// The passes that were created, based on dependencies between tasks
    pub passes: Vec<Pass>,

    /// Current frame id, used for debug validation
    frame_id: FrameId,

    /// GPU specific data for each task that is made available to shaders
    pub task_data: Vec<RenderTaskData>,

    /// Total number of intermediate surfaces that will be drawn to, used for test validation.
    #[cfg(test)]
    surface_count: usize,

    /// Total number of real allocated textures that will be drawn to, used for test validation.
    #[cfg(test)]
    unique_surfaces: FastHashSet<CacheTextureId>,
}

/// The persistent interface that is used during frame building to construct the
/// frame graph.
pub struct FrameGraphBuilder {
    /// List of tasks added to the builder
    tasks: Vec<RenderTask>,

    /// List of task roots
    roots: FastHashSet<RenderTaskId>,

    /// Input dependencies where the input is a persistent target,
    /// rather than a specific render task id. Useful for expressing
    /// when a task relies on a readback of a surface that is partially
    /// drawn to.
    target_inputs: Vec<(RenderTaskId, StaticRenderTaskSurface)>,

    /// Current frame id, used for debug validation
    frame_id: FrameId,

    /// A list of texture surfaces that can be freed at the end of a pass. Retained
    /// here to reduce heap allocations.
    textures_to_free: FastHashSet<CacheTextureId>,

    // Keep a map of `texture_id` to metadata about surfaces that are currently
    // borrowed from the render target pool.
    active_surfaces: FastHashMap<CacheTextureId, Surface>,

    /// A temporary buffer used by assign_free_pass. Kept here to avoid heap reallocs
    child_task_buffer: Vec<RenderTaskId>,
}

impl FrameGraphBuilder {
    /// Construct a new graph builder. Typically constructed once and maintained
    /// over many frames, to avoid extra heap allocations where possible.
    pub fn new() -> Self {
        FrameGraphBuilder {
            tasks: Vec::new(),
            roots: FastHashSet::default(),
            target_inputs: Vec::new(),
            frame_id: FrameId::INVALID,
            textures_to_free: FastHashSet::default(),
            active_surfaces: FastHashMap::default(),
            child_task_buffer: Vec::new(),
        }
    }

    pub fn frame_id(&self) -> FrameId {
        self.frame_id
    }

    /// Begin a new frame
    pub fn begin_frame(&mut self, frame_id: FrameId) {
        self.frame_id = frame_id;
        self.roots.clear();
    }

    /// Get immutable access to a task
    // TODO(gw): There's only a couple of places that existing code needs to access
    //           a task during the building step. Perhaps we can remove this?
    pub fn get_task(
        &self,
        task_id: RenderTaskId,
    ) -> &RenderTask {
        &self.tasks[task_id.index as usize]
    }

    /// Get mutable access to a task
    // TODO(gw): There's only a couple of places that existing code needs to access
    //           a task during the building step. Perhaps we can remove this?
    pub fn get_task_mut(
        &mut self,
        task_id: RenderTaskId,
    ) -> &mut RenderTask {
        &mut self.tasks[task_id.index as usize]
    }

    /// Add a new task to the graph.
    pub fn add(&mut self) -> RenderTaskAllocation {
        // Assume every task is a root to start with
        self.roots.insert(
            RenderTaskId { index: self.tasks.len() as u32 }
        );

        RenderTaskAllocation {
            alloc: self.tasks.alloc(),
        }
    }

    /// Express a dependency, such that `task_id` depends on `input` as a texture source.
    pub fn add_dependency(
        &mut self,
        task_id: RenderTaskId,
        input: RenderTaskId,
    ) {
        self.tasks[task_id.index as usize].children.push(input);

        // Once a task is an input, it's no longer a root
        self.roots.remove(&input);
    }

    /// Register a persistent surface as an input dependency of a task (readback).
    pub fn add_target_input(
        &mut self,
        task_id: RenderTaskId,
        target: StaticRenderTaskSurface,
    ) {
        self.target_inputs.push((task_id, target));
    }

    /// End the graph building phase and produce the immutable task graph for this frame
    pub fn end_frame(
        &mut self,
        resource_cache: &mut ResourceCache,
        gpu_cache: &mut GpuCache,
        deferred_resolves: &mut Vec<DeferredResolve>,
    ) -> FrameGraph {
        // Copy the render tasks over to the immutable graph output
        let task_count = self.tasks.len();
        let tasks = mem::replace(
            &mut self.tasks,
            Vec::with_capacity(task_count),
        );

        let mut graph = FrameGraph {
            tasks,
            passes: Vec::new(),
            task_data: Vec::with_capacity(task_count),
            frame_id: self.frame_id,
            #[cfg(test)]
            surface_count: 0,
            #[cfg(test)]
            unique_surfaces: FastHashSet::default(),
        };

        // Handle late mapping of dependencies on a specific persistent target.
        // NOTE: This functionality isn't used by current callers of the frame graph, but
        //       will be used in future (for example, to express readbacks of partially
        //       rendered picture tiles for mix-blend-mode etc).
        if !self.target_inputs.is_empty() {
            // Create a mapping from persistent surface id -> render task root (used below):
            let mut roots = FastHashMap::default();
            roots.reserve(self.roots.len());
            for root_id in &self.roots {
                let task = &graph.tasks[root_id.index as usize];
                match task.location {
                    RenderTaskLocation::Static { ref surface, .. } => {
                        // We should never encounter a graph where the same surface is a
                        // render root more than one.
                        assert!(!roots.contains_key(surface));
                        roots.insert(surface.clone(), *root_id);
                    }
                    RenderTaskLocation::Dynamic { .. }
                    | RenderTaskLocation::CacheRequest { .. }
                    | RenderTaskLocation::Unallocated { .. } => {
                        // Intermediate surfaces can't be render roots, they should always
                        // be a dependency of a render root.
                        panic!("bug: invalid root");
                    }
                }
            }
            assert_eq!(roots.len(), self.roots.len());

            // Now resolve those dependencies on persistent targets and add them
            // as a render task dependency.
            for (task_id, target_id) in self.target_inputs.drain(..) {
                match roots.get(&target_id) {
                    Some(root_task_id) => {
                        graph.tasks[task_id.index as usize].children.push(*root_task_id);
                        self.roots.remove(root_task_id);
                    }
                    None => {
                        println!("WARN: {:?} depends on root {:?} but it has no tasks!",
                            task_id,
                            target_id,
                        );
                    }
                }
            }
        }

        // Two traversals of the graph are required. The first pass determines how many passes
        // are required, and assigns render tasks a pass to be drawn on. The second pass determines
        // when the last time a render task is used as an input, and assigns what pass the surface
        // backing that render task can be freed (the surface is then returned to the render target
        // pool and may be aliased / reused during subsequent passes).

        let mut pass_count = 0;

        // Traverse each root, and assign `render_on` for each task and count number of required passes
        for root_id in &self.roots {
            assign_render_pass(
                *root_id,
                PassId(0),
                &mut graph,
                &mut pass_count,
            );
        }

        // Determine which pass each task can be freed on, which depends on which is
        // the last task that has this as an input.
        for i in 0 .. graph.tasks.len() {
            let task_id = RenderTaskId { index: i as u32 };
            assign_free_pass(
                task_id,
                &mut self.child_task_buffer,
                &mut graph,
            );
        }

        // Construct passes array for tasks to be assigned to below
        for _ in 0 .. pass_count+1 {
            graph.passes.push(Pass {
                task_ids: Vec::new(),
                sub_passes: Vec::new(),
                textures_to_invalidate: Vec::new(),
            });
        }

        // Assign tasks to each pass based on their `render_on` attribute
        for (index, task) in graph.tasks.iter().enumerate() {
            if task.kind.is_a_rendering_operation() {
                let id = RenderTaskId { index: index as u32 };
                graph.passes[task.render_on.0].task_ids.push(id);
            }
        }

        // At this point, tasks are assigned to each dependency pass. Now we
        // can go through each pass and create sub-passes, assigning each task
        // to a target and destination rect.
        assert!(self.active_surfaces.is_empty());

        for (pass_id, pass) in graph.passes.iter_mut().enumerate().rev() {
            assert!(self.textures_to_free.is_empty());

            for task_id in &pass.task_ids {
                let task = &mut graph.tasks[task_id.index as usize];

                match task.location {
                    RenderTaskLocation::Unallocated { size } => {
                        let mut location = None;
                        let kind = task.kind.target_kind();

                        // Allow this render task to use a shared surface target if it
                        // is freed straight after this pass. Tasks that must remain
                        // allocated for inputs on subsequent passes are always assigned
                        // to a standalone surface, to simplify lifetime management of
                        // render targets.

                        let can_use_shared_surface =
                            task.render_on == PassId(task.free_after.0 + 1);

                        if can_use_shared_surface {
                            // If we can use a shared surface, step through the existing shared
                            // surfaces for this subpass, and see if we can allocate the task
                            // to one of these targets.
                            for sub_pass in &mut pass.sub_passes {
                                if let SubPassSurface::Dynamic { texture_id, ref mut used_rect, .. } = sub_pass.surface {
                                    let surface = self.active_surfaces.get_mut(&texture_id).unwrap();
                                    if let Some(p) = surface.alloc_rect(size, kind, true) {
                                        location = Some((texture_id, p));
                                        *used_rect = used_rect.union(&DeviceIntRect::new(p, size));
                                        sub_pass.task_ids.push(*task_id);
                                        break;
                                    }
                                }
                            }
                        }

                        if location.is_none() {
                            // If it wasn't possible to allocate the task to a shared surface, get a new
                            // render target from the resource cache pool/

                            // If this is a really large task, don't bother allocating it as a potential
                            // shared surface for other tasks.

                            let can_use_shared_surface = can_use_shared_surface &&
                                size.width <= MAX_SHARED_SURFACE_SIZE &&
                                size.height <= MAX_SHARED_SURFACE_SIZE;

                            let surface_size = if can_use_shared_surface {
                                DeviceIntSize::new(
                                    MAX_SHARED_SURFACE_SIZE,
                                    MAX_SHARED_SURFACE_SIZE,
                                )
                            } else {
                                // Round up size here to avoid constant re-allocs during resizing
                                DeviceIntSize::new(
                                    (size.width + TEXTURE_DIMENSION_MASK) & !TEXTURE_DIMENSION_MASK,
                                    (size.height + TEXTURE_DIMENSION_MASK) & !TEXTURE_DIMENSION_MASK,
                                )
                            };

                            let format = match kind {
                                RenderTargetKind::Color => ImageFormat::RGBA8,
                                RenderTargetKind::Alpha => ImageFormat::R8,
                            };

                            // Get render target of appropriate size and format from resource cache
                            let texture_id = resource_cache.get_or_create_render_target_from_pool(
                                surface_size,
                                format,
                            );

                            // Allocate metadata we need about this surface while it's active
                            let mut surface = Surface {
                                kind,
                                allocator: GuillotineAllocator::new(Some(surface_size)),
                                is_shared: can_use_shared_surface,
                            };

                            // Allocation of the task must fit in this new surface!
                            let p = surface.alloc_rect(
                                size,
                                kind,
                                can_use_shared_surface,
                            ).expect("bug: alloc must succeed!");

                            location = Some((texture_id, p));

                            // Store the metadata about this newly active surface. We should never
                            // get a target surface with the same texture_id as a currently active surface.
                            let _prev_surface = self.active_surfaces.insert(texture_id, surface);
                            assert!(_prev_surface.is_none());

                            // Store some information about surface allocations if in test mode
                            #[cfg(test)]
                            {
                                graph.surface_count += 1;
                                graph.unique_surfaces.insert(texture_id);
                            }

                            // Add the target as a new subpass for this render pass.
                            pass.sub_passes.push(SubPass {
                                surface: SubPassSurface::Dynamic {
                                    texture_id,
                                    target_kind: kind,
                                    used_rect: DeviceIntRect::new(p, size),
                                },
                                task_ids: vec![*task_id],
                            });
                        }

                        // By now, we must have allocated a surface and rect for this task, so assign it!
                        assert!(location.is_some());
                        task.location = RenderTaskLocation::Dynamic {
                            texture_id: location.unwrap().0,
                            rect: DeviceIntRect::new(location.unwrap().1, size),
                        };
                    }
                    RenderTaskLocation::Static { ref surface, .. } => {
                        // No need to allocate for this surface, since it's a persistent
                        // target. Instead, just create a new sub-pass for it.
                        pass.sub_passes.push(SubPass {
                            surface: SubPassSurface::Persistent {
                                surface: surface.clone(),
                            },
                            task_ids: vec![*task_id],
                        });
                    }
                    RenderTaskLocation::CacheRequest { .. } => {
                        // No need to allocate nor to create a sub-path for read-only locations.
                    }
                    RenderTaskLocation::Dynamic { .. } => {
                        // Dynamic tasks shouldn't be allocated by this point
                        panic!("bug: encountered an already allocated task");
                    }
                }

                // Return the shared surfaces from this pass
                let task = &graph.tasks[task_id.index as usize];
                for child_id in &task.children {
                    let child_task = &graph.tasks[child_id.index as usize];
                    match child_task.location {
                        RenderTaskLocation::Unallocated { .. } => panic!("bug: must be allocated"),
                        RenderTaskLocation::Dynamic { texture_id, .. } => {
                            // If this task can be freed after this pass, include it in the
                            // unique set of textures to be returned to the render target pool below.
                            if child_task.free_after == PassId(pass_id) {
                                self.textures_to_free.insert(texture_id);
                            }
                        }
                        RenderTaskLocation::Static { .. } => {}
                        RenderTaskLocation::CacheRequest { .. } => {}
                    }
                }
            }

            // Return no longer used textures to the pool, so that they can be reused / aliased
            // by later passes.
            for texture_id in self.textures_to_free.drain() {
                resource_cache.return_render_target_to_pool(texture_id);
                self.active_surfaces.remove(&texture_id).unwrap();
                pass.textures_to_invalidate.push(texture_id);
            }
        }

        // By now, all surfaces that were borrowed from the render target pool must
        // be returned to the resource cache, or we are leaking intermediate surfaces!
        assert!(self.active_surfaces.is_empty());

        // Each task is now allocated to a surface and target rect. Write that to the
        // GPU blocks and task_data. After this point, the graph is returned and is
        // considered to be immutable for the rest of the frame building process.

        for task in &mut graph.tasks {
            if let RenderTaskKind::Image(request) = task.kind {
                let cache_item = resolve_image(
                    request,
                    resource_cache,
                    gpu_cache,
                    deferred_resolves,
                );

                task.uv_rect_handle = Some(cache_item.uv_rect_handle);
                task.location = RenderTaskLocation::Static {
                    surface: StaticRenderTaskSurface::ReadOnly {
                        source: cache_item.texture_id,
                    },
                    rect: cache_item.uv_rect,
                };
            }

            // Give the render task an opportunity to add any
            // information to the GPU cache, if appropriate.
            let (target_rect, target_index) = task.get_target_rect();

            task.write_gpu_blocks(
                target_rect,
                target_index,
                gpu_cache,
            );

            graph.task_data.push(
                task.kind.write_task_data(
                    target_rect,
                    target_index,
                )
            );
        }

        graph
    }
}

impl FrameGraph {
    /// Print the render task graph to console
    #[allow(dead_code)]
    pub fn print(
        &self,
    ) {
        println!("-- FrameGraph --");

        for (i, task) in self.tasks.iter().enumerate() {
            println!("Task {}: render_on={} free_after={} {:?}",
                i,
                task.render_on.0,
                task.free_after.0,
                task.kind.as_str(),
            );
        }

        for (p, pass) in self.passes.iter().enumerate() {
            println!("Pass {}:", p);

            for (s, sub_pass) in pass.sub_passes.iter().enumerate() {
                println!("\tSubPass {}: {:?}",
                    s,
                    sub_pass.surface,
                );

                for task_id in &sub_pass.task_ids {
                    println!("\t\tTask {:?}", task_id.index);
                }
            }
        }
    }

    /// Return the surface and texture counts, used for testing
    #[cfg(test)]
    pub fn surface_counts(&self) -> (usize, usize) {
        (self.surface_count, self.unique_surfaces.len())
    }

    /// Return current frame id, used for validation
    #[cfg(debug_assertions)]
    pub fn frame_id(&self) -> FrameId {
        self.frame_id
    }
}

/// Batching uses index access to read information about tasks
impl std::ops::Index<RenderTaskId> for FrameGraph {
    type Output = RenderTask;
    fn index(&self, id: RenderTaskId) -> &RenderTask {
        &self.tasks[id.index as usize]
    }
}

/// Recursive helper to assign pass that a task should render on
fn assign_render_pass(
    id: RenderTaskId,
    pass: PassId,
    graph: &mut FrameGraph,
    pass_count: &mut usize,
) {
    let task = &mut graph.tasks[id.index as usize];

    // No point in recursing into paths in the graph if this task already
    // has been set to draw after this pass.
    if task.render_on > pass {
        return;
    }

    let next_pass = if task.kind.is_a_rendering_operation() {
        // Keep count of number of passes needed
        *pass_count = pass.0.max(*pass_count);
        PassId(pass.0 + 1)
    } else {
        // If the node is not a rendering operation, it doesn't create a
        // render pass, so we don't increment the pass count. 
        // For now we expect non-rendering nodes to be leafs of the graph.
        // We don't strictly depend on it but it simplifies the mental model.
        debug_assert!(task.children.is_empty());
        pass
    };

    // A task should be rendered on the earliest pass in the dependency
    // graph that it's required. Using max here ensures the correct value
    // in the presence of multiple paths to this task from the root(s).
    task.render_on = task.render_on.max(pass);

    // TODO(gw): Work around the borrowck - maybe we could structure the dependencies
    //           storage better, to avoid this?
    let mut child_task_ids: SmallVec<[RenderTaskId; 8]> = SmallVec::new();
    child_task_ids.extend_from_slice(&task.children);

    for child_id in child_task_ids {
        assign_render_pass(
            child_id,
            next_pass,
            graph,
            pass_count,
        );
    }
}

fn assign_free_pass(
    id: RenderTaskId,
    child_task_buffer: &mut Vec<RenderTaskId>,
    graph: &mut FrameGraph,
) {
    let task = &graph.tasks[id.index as usize];
    let render_on = task.render_on;
    debug_assert!(child_task_buffer.is_empty());

    // TODO(gw): Work around the borrowck - maybe we could structure the dependencies
    //           storage better, to avoid this?
    child_task_buffer.extend_from_slice(&task.children);

    for child_id in child_task_buffer.drain(..) {
        let child_task = &mut graph.tasks[child_id.index as usize];

        // Each dynamic child task can free its backing surface after the last
        // task that references it as an input. Using min here ensures the
        // safe time to free this surface in the presence of multiple paths
        // to this task from the root(s).
        match child_task.location {
            RenderTaskLocation::CacheRequest { .. } => {}
            RenderTaskLocation::Static { .. } => {
                // never get freed anyway, so can leave untouched
                // (could validate that they remain at PassId::MIN)
            }
            RenderTaskLocation::Unallocated { .. } => {
                child_task.free_after = child_task.free_after.min(render_on);
            }
            RenderTaskLocation::Dynamic { .. } => {
                panic!("bug: should not be allocated yet");
            }
        }
    }
}


/// Construct a picture cache render task location for testing
#[cfg(test)]
fn pc_target(
    surface_id: u64,
    tile_x: i32,
    tile_y: i32,
) -> RenderTaskLocation {
    use crate::{
        composite::{NativeSurfaceId, NativeTileId},
        picture::ResolvedSurfaceTexture,
    };

    let width = 512;
    let height = 512;

    RenderTaskLocation::Static {
        surface: StaticRenderTaskSurface::PictureCache {
            surface: ResolvedSurfaceTexture::Native {
                id: NativeTileId {
                    surface_id: NativeSurfaceId(surface_id),
                    x: tile_x,
                    y: tile_y,
                },
                size: DeviceIntSize::new(width, height),
            },
        },
        rect: DeviceIntSize::new(width, height).into(),
    }
}

#[cfg(test)]
impl FrameGraphBuilder {
    fn test_expect(
        mut self,
        pass_count: usize,
        total_surface_count: usize,
        unique_surfaces: &[(i32, i32, ImageFormat)],
    ) {
        let mut rc = ResourceCache::new_for_testing();
        let mut gc =  GpuCache::new();

        let g = self.end_frame(&mut rc, &mut gc, &mut Vec::new());
        g.print();

        assert_eq!(g.passes.len(), pass_count);
        assert_eq!(g.surface_counts(), (total_surface_count, unique_surfaces.len()));

        rc.validate_surfaces(unique_surfaces);
    }
}

/// Construct a testing render task with given location
#[cfg(test)]
fn task_location(location: RenderTaskLocation) -> RenderTask {
    RenderTask::new_test(
        location,
        RenderTargetKind::Color,
    )
}

/// Construct a dynamic render task location for testing
#[cfg(test)]
fn task_dynamic(size: i32) -> RenderTask {
    RenderTask::new_test(
        RenderTaskLocation::Unallocated { size: DeviceIntSize::new(size, size) },
        RenderTargetKind::Color,
    )
}

#[test]
fn fg_test_1() {
    // Test that a root target can be used as an input for readbacks
    // This functionality isn't currently used, but will be in future.

    let mut gb = FrameGraphBuilder::new();

    let root_target = pc_target(0, 0, 0);

    let root = gb.add().init(task_location(root_target.clone()));

    let readback = gb.add().init(task_dynamic(100));
    gb.add_dependency(readback, root);

    let mix_blend_content = gb.add().init(task_dynamic(50));

    let content = gb.add().init(task_location(root_target));
    gb.add_dependency(content, readback);
    gb.add_dependency(content, mix_blend_content);

    gb.test_expect(3, 1, &[
        (2048, 2048, ImageFormat::RGBA8),
    ]);
}

#[test]
fn fg_test_2() {
    // Test that texture cache tasks can be added and scheduled correctly as inputs
    // to picture cache tasks. Ensure that no dynamic surfaces are allocated from the
    // target pool in this case.

    let mut gb = FrameGraphBuilder::new();

    let pc_root = gb.add().init(task_location(pc_target(0, 0, 0)));

    let tc_0 = StaticRenderTaskSurface::TextureCache {
        texture: CacheTextureId(0),
        layer: 0,
        target_kind: RenderTargetKind::Color,
    };

    let tc_1 = StaticRenderTaskSurface::TextureCache {
        texture: CacheTextureId(1),
        layer: 0,
        target_kind: RenderTargetKind::Color,
    };

    gb.add_target_input(
        pc_root,
        tc_0.clone(),
    );

    gb.add_target_input(
        pc_root,
        tc_1.clone(),
    );

    gb.add().init(
        task_location(RenderTaskLocation::Static { surface: tc_0.clone(), rect: DeviceIntSize::new(128, 128).into() }),
    );

    gb.add().init(
        task_location(RenderTaskLocation::Static { surface: tc_1.clone(), rect: DeviceIntSize::new(128, 128).into() }),
    );

    gb.test_expect(2, 0, &[]);
}

#[test]
fn fg_test_3() {
    // Test that small targets are allocated in a shared surface, and that large
    // tasks are allocated in a rounded up texture size.

    let mut gb = FrameGraphBuilder::new();

    let pc_root = gb.add().init(task_location(pc_target(0, 0, 0)));

    let child_pic_0 = gb.add().init(task_dynamic(128));
    let child_pic_1 = gb.add().init(task_dynamic(3000));

    gb.add_dependency(pc_root, child_pic_0);
    gb.add_dependency(pc_root, child_pic_1);

    gb.test_expect(2, 2, &[
        (2048, 2048, ImageFormat::RGBA8),
        (3072, 3072, ImageFormat::RGBA8),
    ]);
}

#[test]
fn fg_test_4() {
    // Test that for a simple dependency chain of tasks, that render
    // target surfaces are aliased and reused between passes where possible.

    let mut gb = FrameGraphBuilder::new();

    let pc_root = gb.add().init(task_location(pc_target(0, 0, 0)));

    let child_pic_0 = gb.add().init(task_dynamic(128));
    let child_pic_1 = gb.add().init(task_dynamic(128));
    let child_pic_2 = gb.add().init(task_dynamic(128));

    gb.add_dependency(pc_root, child_pic_0);
    gb.add_dependency(child_pic_0, child_pic_1);
    gb.add_dependency(child_pic_1, child_pic_2);

    gb.test_expect(4, 3, &[
        (2048, 2048, ImageFormat::RGBA8),
        (2048, 2048, ImageFormat::RGBA8),
    ]);
}

#[test]
fn fg_test_5() {
    // Test that a task that is used as an input by direct parent and also
    // distance ancestor are scheduled correctly, and allocates the correct
    // number of passes, taking advantage of surface reuse / aliasing where feasible.

    let mut gb = FrameGraphBuilder::new();

    let pc_root = gb.add().init(task_location(pc_target(0, 0, 0)));

    let child_pic_0 = gb.add().init(task_dynamic(128));
    let child_pic_1 = gb.add().init(task_dynamic(64));
    let child_pic_2 = gb.add().init(task_dynamic(32));
    let child_pic_3 = gb.add().init(task_dynamic(16));

    gb.add_dependency(pc_root, child_pic_0);
    gb.add_dependency(child_pic_0, child_pic_1);
    gb.add_dependency(child_pic_1, child_pic_2);
    gb.add_dependency(child_pic_2, child_pic_3);
    gb.add_dependency(pc_root, child_pic_3);

    gb.test_expect(5, 4, &[
        (256, 256, ImageFormat::RGBA8),
        (2048, 2048, ImageFormat::RGBA8),
        (2048, 2048, ImageFormat::RGBA8),
    ]);
}

#[test]
fn fg_test_6() {
    // Test that a task that is used as an input dependency by two parent
    // tasks is correctly allocated and freed.

    let mut gb = FrameGraphBuilder::new();

    let pc_root_1 = gb.add().init(task_location(pc_target(0, 0, 0)));
    let pc_root_2 = gb.add().init(task_location(pc_target(0, 1, 0)));

    let child_pic = gb.add().init(task_dynamic(128));

    gb.add_dependency(pc_root_1, child_pic);
    gb.add_dependency(pc_root_2, child_pic);

    gb.test_expect(2, 1, &[
        (2048, 2048, ImageFormat::RGBA8),
    ]);
}

#[test]
fn fg_test_7() {
    // Test that a standalone surface is not incorrectly used to
    // allocate subsequent shared task rects.

    let mut gb = FrameGraphBuilder::new();

    let pc_root = gb.add().init(task_location(pc_target(0, 0, 0)));

    let child0 = gb.add().init(task_dynamic(16));
    let child1 = gb.add().init(task_dynamic(16));

    let child2 = gb.add().init(task_dynamic(16));
    let child3 = gb.add().init(task_dynamic(16));

    gb.add_dependency(pc_root, child0);
    gb.add_dependency(child0, child1);
    gb.add_dependency(pc_root, child1);

    gb.add_dependency(pc_root, child2);
    gb.add_dependency(child2, child3);

    gb.test_expect(3, 3, &[
        (256, 256, ImageFormat::RGBA8),
        (2048, 2048, ImageFormat::RGBA8),
        (2048, 2048, ImageFormat::RGBA8),
    ]);
}
