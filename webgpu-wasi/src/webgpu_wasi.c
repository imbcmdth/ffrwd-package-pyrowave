// webgpu.h over wasi:webgpu@0.0.1, for C and C++ guests built with wasi-sdk.
//
// Covers the compute side of webgpu.h: instance, adapter, device, queue,
// buffers (with mapping), textures and views, samplers, shaders, bind groups
// and layouts, compute pipelines and passes, query sets and error scopes.
// Render passes, render pipelines, surfaces and the async pipeline creation
// entry points are not here; a module that calls one fails to link rather
// than failing at run time.
//
// Waiting. wasi:webgpu@0.0.1 is the WASI 0.2 version of the interface: the
// calls a browser makes asynchronous (request-adapter, request-device,
// map-async, pop-error-scope, on-submitted-work-done) are blocking calls
// that return once the host has the answer. So every operation that returns a
// WGPUFuture here has already completed when it returns, and what webgpu.h's
// callback modes still decide is WHEN the callback runs:
//
// - WGPUCallbackMode_AllowSpontaneous: at once, inside the call.
// - WGPUCallbackMode_AllowProcessEvents: in the next wgpuInstanceProcessEvents,
//   or in a wgpuInstanceWaitAny naming the future, whichever comes first.
// - WGPUCallbackMode_WaitAnyOnly: in a wgpuInstanceWaitAny naming the future.
//
// wgpuInstanceProcessEvents never blocks, and wgpuInstanceWaitAny never has
// to (every future it is handed is already complete), so the instance
// feature TimedWaitAny is offered and any timeout is honoured trivially. The
// one WASI pollable 0.0.1 has, a device's uncaptured-error subscription, is
// checked (without blocking) in wgpuInstanceProcessEvents and
// wgpuInstanceWaitAny, and each time it is ready the device's uncaptured
// error callback runs. 0.0.1 cannot say what the error was, only that one
// happened; the host prints it.
//
// Mapping. A mapped range lives in linear memory: mapping for reading copies
// the range out of the host when the guest first asks for it
// (get-mapped-range-get-with-copy), and a range mapped for writing, or at
// creation, is a zeroed guest buffer written back to the host on unmap
// (get-mapped-range-set-with-copy).
//
// Threads: none. Like the rest of webgpu.h, nothing here is safe to call
// from two threads at once, and wasi-sdk's wasm32-wasip2 has none anyway.

#include "webgpu/webgpu.h"
#include "webgpu/wasi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enums.h"
#include "webgpu_wasi.h"

// ---------------------------------------------------------------------------
// Helpers

static void fatal(const char* what) {
    fprintf(stderr, "webgpu-wasi: %s\n", what);
    abort();
}

static void* xmalloc(size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) fatal("out of memory");
    return p;
}

static void* xcalloc(size_t count, size_t size) {
    void* p = calloc(count ? count : 1, size ? size : 1);
    if (!p) fatal("out of memory");
    return p;
}

// A WGPUStringView as a borrowed WIT string: no copy, the call only reads it.
static webgpu_wasi_string_t str_in(WGPUStringView view) {
    webgpu_wasi_string_t s;
    s.ptr = (uint8_t*)view.data;
    if (!view.data) s.len = 0;
    else if (view.length == WGPU_STRLEN) s.len = strlen(view.data);
    else s.len = view.length;
    return s;
}

static webgpu_wasi_string_t str_lit(const char* text) {
    webgpu_wasi_string_t s = {(uint8_t*)text, strlen(text)};
    return s;
}

// A label: absent when empty, as WebGPU's defaults are.
static webgpu_wasi_option_string_t label_in(WGPUStringView view) {
    webgpu_wasi_option_string_t label = {0};
    webgpu_wasi_string_t s = str_in(view);
    if (s.len) {
        label.is_some = true;
        label.val = s;
    }
    return label;
}

// A string the host returned, as a view that owns a NUL-terminated copy the
// caller frees with free(). The WIT string itself is freed here.
static WGPUStringView str_out_owned(webgpu_wasi_string_t* s) {
    char* copy = xmalloc(s->len + 1);
    if (s->len) memcpy(copy, s->ptr, s->len);
    copy[s->len] = '\0';
    WGPUStringView view = {copy, s->len};
    webgpu_wasi_string_free(s);
    return view;
}

static WGPUStringView str_view(const char* text) {
    WGPUStringView view = {text, text ? strlen(text) : 0};
    return view;
}

// ---------------------------------------------------------------------------
// Objects

typedef struct Event Event;

typedef struct WGPUInstanceImpl {
    uint32_t refCount;
    wasi_webgpu_webgpu_own_gpu_t gpu;
    uint64_t nextFutureId;
    // Completed operations whose callbacks have not run yet, oldest first.
    Event* head;
    Event* tail;
    // Devices that subscribed to uncaptured errors, for the event loop.
    struct WGPUDeviceImpl* devices;
} WGPUInstanceImpl;

typedef struct WGPUAdapterImpl {
    uint32_t refCount;
    WGPUInstanceImpl* instance;
    wasi_webgpu_webgpu_own_gpu_adapter_t adapter;
} WGPUAdapterImpl;

typedef struct WGPUDeviceImpl {
    uint32_t refCount;
    WGPUInstanceImpl* instance;
    WGPUAdapterImpl* adapter;
    wasi_webgpu_webgpu_own_gpu_device_t device;
    struct WGPUQueueImpl* queue;
    WGPUDeviceLostCallbackInfo lost;
    WGPUUncapturedErrorCallbackInfo uncaptured;
    bool hasErrorPollable;
    webgpu_wasi_poll_own_pollable_t errorPollable;
    bool lostFired;
    struct WGPUDeviceImpl* nextDevice;
} WGPUDeviceImpl;

typedef struct WGPUQueueImpl {
    uint32_t refCount;
    WGPUInstanceImpl* instance;
    wasi_webgpu_webgpu_own_gpu_queue_t queue;
} WGPUQueueImpl;

typedef struct WGPUBufferImpl {
    uint32_t refCount;
    WGPUInstanceImpl* instance;
    wasi_webgpu_webgpu_own_gpu_buffer_t buffer;
    uint64_t size;
    WGPUBufferUsage usage;
    WGPUBufferMapState mapState;
    WGPUMapMode mapMode;  // WGPUMapMode_Write for mapped-at-creation
    uint64_t mapOffset;
    uint64_t mapSize;
    // The mapped range's bytes in linear memory, once fetched (read) or made
    // (write). Covers [mapOffset, mapOffset + mapSize).
    uint8_t* shadow;
} WGPUBufferImpl;

typedef struct WGPUTextureImpl {
    uint32_t refCount;
    wasi_webgpu_webgpu_own_gpu_texture_t texture;
    WGPUTextureDescriptor desc;  // label and view formats not kept
} WGPUTextureImpl;

#define SIMPLE_OBJECT(Name, handle_type)       \
    typedef struct WGPU##Name##Impl {          \
        uint32_t refCount;                     \
        wasi_webgpu_webgpu_own_##handle_type##_t handle; \
    } WGPU##Name##Impl;

SIMPLE_OBJECT(TextureView, gpu_texture_view)
SIMPLE_OBJECT(Sampler, gpu_sampler)
SIMPLE_OBJECT(BindGroupLayout, gpu_bind_group_layout)
SIMPLE_OBJECT(BindGroup, gpu_bind_group)
SIMPLE_OBJECT(PipelineLayout, gpu_pipeline_layout)
SIMPLE_OBJECT(ShaderModule, gpu_shader_module)
SIMPLE_OBJECT(ComputePipeline, gpu_compute_pipeline)
SIMPLE_OBJECT(CommandEncoder, gpu_command_encoder)
SIMPLE_OBJECT(CommandBuffer, gpu_command_buffer)
SIMPLE_OBJECT(ComputePassEncoder, gpu_compute_pass_encoder)

typedef struct WGPUQuerySetImpl {
    uint32_t refCount;
    wasi_webgpu_webgpu_own_gpu_query_set_t handle;
    WGPUQueryType type;
    uint32_t count;
} WGPUQuerySetImpl;

// AddRef and Release for the objects that only hold their host handle.
#define SIMPLE_REFCOUNT(Name, drop)                                  \
    void wgpu##Name##AddRef(WGPU##Name object) {                     \
        if (!object) fatal("wgpu" #Name "AddRef on NULL");           \
        object->refCount++;                                          \
    }                                                                \
    void wgpu##Name##Release(WGPU##Name object) {                    \
        if (!object) fatal("wgpu" #Name "Release on NULL");          \
        if (object->refCount == 0) fatal("wgpu" #Name "Release past zero"); \
        if (--object->refCount == 0) {                               \
            drop(object->handle);                                    \
            free(object);                                            \
        }                                                            \
    }

SIMPLE_REFCOUNT(TextureView, wasi_webgpu_webgpu_gpu_texture_view_drop_own)
SIMPLE_REFCOUNT(Sampler, wasi_webgpu_webgpu_gpu_sampler_drop_own)
SIMPLE_REFCOUNT(BindGroupLayout, wasi_webgpu_webgpu_gpu_bind_group_layout_drop_own)
SIMPLE_REFCOUNT(BindGroup, wasi_webgpu_webgpu_gpu_bind_group_drop_own)
SIMPLE_REFCOUNT(PipelineLayout, wasi_webgpu_webgpu_gpu_pipeline_layout_drop_own)
SIMPLE_REFCOUNT(ShaderModule, wasi_webgpu_webgpu_gpu_shader_module_drop_own)
SIMPLE_REFCOUNT(ComputePipeline, wasi_webgpu_webgpu_gpu_compute_pipeline_drop_own)
SIMPLE_REFCOUNT(CommandEncoder, wasi_webgpu_webgpu_gpu_command_encoder_drop_own)
SIMPLE_REFCOUNT(CommandBuffer, wasi_webgpu_webgpu_gpu_command_buffer_drop_own)
SIMPLE_REFCOUNT(ComputePassEncoder, wasi_webgpu_webgpu_gpu_compute_pass_encoder_drop_own)
SIMPLE_REFCOUNT(QuerySet, wasi_webgpu_webgpu_gpu_query_set_drop_own)

#define BORROW(kind, object) wasi_webgpu_webgpu_borrow_##kind(object)

// ---------------------------------------------------------------------------
// Events: completed operations waiting for their callback to be allowed to run

typedef enum EventKind {
    EVENT_REQUEST_ADAPTER,
    EVENT_REQUEST_DEVICE,
    EVENT_MAP,
    EVENT_POP_ERROR_SCOPE,
    EVENT_WORK_DONE,
} EventKind;

struct Event {
    uint64_t id;
    EventKind kind;
    WGPUCallbackMode mode;
    void* callback;
    void* userdata1;
    void* userdata2;
    int status;
    int errorType;
    void* object;
    char* message;  // owned, NUL-terminated, may be NULL
    Event* next;
};

static void event_run(Event* e) {
    WGPUStringView message = str_view(e->message);
    switch (e->kind) {
    case EVENT_REQUEST_ADAPTER:
        ((WGPURequestAdapterCallback)e->callback)(
            (WGPURequestAdapterStatus)e->status, (WGPUAdapter)e->object, message, e->userdata1, e->userdata2);
        break;
    case EVENT_REQUEST_DEVICE:
        ((WGPURequestDeviceCallback)e->callback)(
            (WGPURequestDeviceStatus)e->status, (WGPUDevice)e->object, message, e->userdata1, e->userdata2);
        break;
    case EVENT_MAP:
        ((WGPUBufferMapCallback)e->callback)((WGPUMapAsyncStatus)e->status, message, e->userdata1, e->userdata2);
        break;
    case EVENT_POP_ERROR_SCOPE:
        ((WGPUPopErrorScopeCallback)e->callback)(
            (WGPUPopErrorScopeStatus)e->status, (WGPUErrorType)e->errorType, message, e->userdata1, e->userdata2);
        break;
    case EVENT_WORK_DONE:
        ((WGPUQueueWorkDoneCallback)e->callback)(
            (WGPUQueueWorkDoneStatus)e->status, message, e->userdata1, e->userdata2);
        break;
    }
}

static void event_free(Event* e) {
    free(e->message);
    free(e);
}

// Takes a completed operation: runs its callback now if its mode allows
// that, and queues it for ProcessEvents or WaitAny otherwise. Returns the
// future naming it.
static WGPUFuture event_complete(WGPUInstanceImpl* instance, Event* e) {
    e->id = ++instance->nextFutureId;
    WGPUFuture future = {e->id};
    if (!e->callback) {
        event_free(e);
        return future;
    }
    if (e->mode == WGPUCallbackMode_AllowSpontaneous) {
        event_run(e);
        event_free(e);
        return future;
    }
    e->next = NULL;
    if (instance->tail) instance->tail->next = e;
    else instance->head = e;
    instance->tail = e;
    return future;
}

static Event* event_new(EventKind kind, WGPUCallbackMode mode, void* callback, void* userdata1, void* userdata2) {
    Event* e = xcalloc(1, sizeof(Event));
    e->kind = kind;
    e->mode = mode ? mode : WGPUCallbackMode_AllowProcessEvents;
    e->callback = callback;
    e->userdata1 = userdata1;
    e->userdata2 = userdata2;
    return e;
}

static char* message_copy(const char* text) {
    if (!text) return NULL;
    size_t len = strlen(text);
    char* copy = xmalloc(len + 1);
    memcpy(copy, text, len + 1);
    return copy;
}

static char* message_from_wasi(webgpu_wasi_string_t* s) {
    WGPUStringView view = str_out_owned(s);
    return (char*)view.data;
}

// Removes and returns the queued event for `id`, or NULL.
static Event* event_take(WGPUInstanceImpl* instance, uint64_t id) {
    Event* prev = NULL;
    for (Event* e = instance->head; e; prev = e, e = e->next) {
        if (e->id != id) continue;
        if (prev) prev->next = e->next;
        else instance->head = e->next;
        if (instance->tail == e) instance->tail = prev;
        return e;
    }
    return NULL;
}

// Checks each subscribed device's uncaptured-error pollable without blocking,
// and runs its callback once per error signalled.
static void poll_uncaptured_errors(WGPUInstanceImpl* instance) {
    for (WGPUDeviceImpl* d = instance->devices; d; d = d->nextDevice) {
        if (!d->hasErrorPollable) continue;
        while (webgpu_wasi_poll_method_pollable_ready(webgpu_wasi_poll_borrow_pollable(d->errorPollable))) {
            WGPUDevice device = d;
            d->uncaptured.callback(&device, WGPUErrorType_Unknown,
                                   str_view("uncaptured WebGPU error (wasi:webgpu 0.0.1 does not carry the "
                                            "message; the host printed it)"),
                                   d->uncaptured.userdata1, d->uncaptured.userdata2);
        }
    }
}

// ---------------------------------------------------------------------------
// Instance

WGPUInstance wgpuCreateInstance(WGPU_NULLABLE WGPUInstanceDescriptor const* descriptor) {
    if (descriptor) {
        for (size_t i = 0; i < descriptor->requiredFeatureCount; i++) {
            WGPUInstanceFeatureName f = descriptor->requiredFeatures[i];
            if (f != WGPUInstanceFeatureName_TimedWaitAny) return NULL;
        }
    }
    WGPUInstanceImpl* instance = xcalloc(1, sizeof(WGPUInstanceImpl));
    instance->refCount = 1;
    instance->gpu = wasi_webgpu_webgpu_get_gpu();
    return instance;
}

void wgpuGetInstanceFeatures(WGPUSupportedInstanceFeatures* features) {
    static const WGPUInstanceFeatureName supported[] = {WGPUInstanceFeatureName_TimedWaitAny};
    features->featureCount = 1;
    features->features = supported;
}

WGPUStatus wgpuGetInstanceLimits(WGPUInstanceLimits* limits) {
    if (!limits) return WGPUStatus_Error;
    limits->timedWaitAnyMaxCount = SIZE_MAX;
    return WGPUStatus_Success;
}

WGPUBool wgpuHasInstanceFeature(WGPUInstanceFeatureName feature) {
    return feature == WGPUInstanceFeatureName_TimedWaitAny;
}

void wgpuSupportedInstanceFeaturesFreeMembers(WGPUSupportedInstanceFeatures features) {
    (void)features;  // static storage
}

void wgpuInstanceProcessEvents(WGPUInstance instance) {
    poll_uncaptured_errors(instance);
    // Run what was queued before this call; callbacks that queue more leave it
    // for the next one, so a callback that re-arms cannot spin this forever.
    Event* last = instance->tail;
    if (!last) return;
    Event* e = instance->head;
    Event* kept_head = NULL;
    Event* kept_tail = NULL;
    instance->head = last->next;
    if (!instance->head) instance->tail = NULL;
    last->next = NULL;
    while (e) {
        Event* next = e->next;
        if (e->mode == WGPUCallbackMode_WaitAnyOnly) {
            e->next = NULL;
            if (kept_tail) kept_tail->next = e;
            else kept_head = e;
            kept_tail = e;
        } else {
            event_run(e);
            event_free(e);
        }
        e = next;
    }
    if (kept_head) {
        // WaitAnyOnly events go back to the front, in order.
        kept_tail->next = instance->head;
        instance->head = kept_head;
        if (!instance->tail) instance->tail = kept_tail;
    }
}

WGPUWaitStatus wgpuInstanceWaitAny(WGPUInstance instance, size_t futureCount, WGPU_NULLABLE WGPUFutureWaitInfo* futures,
                                   uint64_t timeoutNS) {
    (void)timeoutNS;  // every future is complete already; nothing to wait for
    poll_uncaptured_errors(instance);
    if (futureCount && !futures) return WGPUWaitStatus_Error;
    bool any = false;
    for (size_t i = 0; i < futureCount; i++) {
        uint64_t id = futures[i].future.id;
        if (id == 0 || id > instance->nextFutureId) return WGPUWaitStatus_Error;
        Event* e = event_take(instance, id);
        if (e) {
            event_run(e);
            event_free(e);
        }
        futures[i].completed = WGPU_TRUE;
        any = true;
    }
    return any ? WGPUWaitStatus_Success : WGPUWaitStatus_TimedOut;
}

WGPUFuture wgpuInstanceRequestAdapter(WGPUInstance instance, WGPU_NULLABLE WGPURequestAdapterOptions const* options,
                                      WGPURequestAdapterCallbackInfo callbackInfo) {
    wasi_webgpu_webgpu_gpu_request_adapter_options_t o = {0};
    if (options) {
        if (options->featureLevel == WGPUFeatureLevel_Compatibility) {
            o.feature_level.is_some = true;
            o.feature_level.val = str_lit("compatibility");
        }
        if (options->powerPreference != WGPUPowerPreference_Undefined) {
            o.power_preference.is_some =
                wgpu_wasi_power_preference_to_wasi(options->powerPreference, &o.power_preference.val);
        }
        if (options->forceFallbackAdapter) {
            o.force_fallback_adapter.is_some = true;
            o.force_fallback_adapter.val = true;
        }
        // backendType has no WIT counterpart: the host picks the backend.
    }
    wasi_webgpu_webgpu_own_gpu_adapter_t handle;
    bool found = wasi_webgpu_webgpu_method_gpu_request_adapter(BORROW(gpu, instance->gpu), options ? &o : NULL, &handle);

    Event* e = event_new(EVENT_REQUEST_ADAPTER, callbackInfo.mode, (void*)callbackInfo.callback, callbackInfo.userdata1,
                         callbackInfo.userdata2);
    if (found) {
        WGPUAdapterImpl* adapter = xcalloc(1, sizeof(WGPUAdapterImpl));
        adapter->refCount = 1;
        adapter->instance = instance;
        adapter->adapter = handle;
        wgpuInstanceAddRef(instance);
        e->status = WGPURequestAdapterStatus_Success;
        e->object = adapter;
    } else {
        e->status = WGPURequestAdapterStatus_Unavailable;
        e->message = message_copy("no GPU adapter is available to this module");
    }
    return event_complete(instance, e);
}

WGPUBool wgpuInstanceHasWGSLLanguageFeature(WGPUInstance instance, WGPUWGSLLanguageFeatureName feature) {
    static const struct {
        WGPUWGSLLanguageFeatureName feature;
        const char* name;
    } names[] = {
        {WGPUWGSLLanguageFeatureName_ReadonlyAndReadwriteStorageTextures, "readonly_and_readwrite_storage_textures"},
        {WGPUWGSLLanguageFeatureName_Packed4x8IntegerDotProduct, "packed_4x8_integer_dot_product"},
        {WGPUWGSLLanguageFeatureName_UnrestrictedPointerParameters, "unrestricted_pointer_parameters"},
        {WGPUWGSLLanguageFeatureName_PointerCompositeAccess, "pointer_composite_access"},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (names[i].feature != feature) continue;
        wasi_webgpu_webgpu_own_wgsl_language_features_t f =
            wasi_webgpu_webgpu_method_gpu_wgsl_language_features(BORROW(gpu, instance->gpu));
        webgpu_wasi_string_t name = str_lit(names[i].name);
        bool has = wasi_webgpu_webgpu_method_wgsl_language_features_has(BORROW(wgsl_language_features, f), &name);
        wasi_webgpu_webgpu_wgsl_language_features_drop_own(f);
        return has;
    }
    return WGPU_FALSE;
}

void wgpuInstanceAddRef(WGPUInstance instance) {
    if (!instance) fatal("wgpuInstanceAddRef on NULL");
    instance->refCount++;
}

void wgpuInstanceRelease(WGPUInstance instance) {
    if (!instance) fatal("wgpuInstanceRelease on NULL");
    if (instance->refCount == 0) fatal("wgpuInstanceRelease past zero");
    if (--instance->refCount) return;
    // Callbacks never delivered are cancelled; webgpu.h says they still run.
    while (instance->head) {
        Event* e = instance->head;
        instance->head = e->next;
        switch (e->kind) {
        case EVENT_REQUEST_ADAPTER: e->status = WGPURequestAdapterStatus_CallbackCancelled; break;
        case EVENT_REQUEST_DEVICE: e->status = WGPURequestDeviceStatus_CallbackCancelled; break;
        case EVENT_MAP: e->status = WGPUMapAsyncStatus_CallbackCancelled; break;
        case EVENT_POP_ERROR_SCOPE: e->status = WGPUPopErrorScopeStatus_CallbackCancelled; break;
        case EVENT_WORK_DONE: e->status = WGPUQueueWorkDoneStatus_CallbackCancelled; break;
        }
        event_run(e);
        event_free(e);
    }
    wasi_webgpu_webgpu_gpu_drop_own(instance->gpu);
    free(instance);
}

// ---------------------------------------------------------------------------
// Limits and features

#define LIMITS_U32(X)                                                                        \
    X(maxTextureDimension1D, max_texture_dimension1_d, "max-texture-dimension1-d")           \
    X(maxTextureDimension2D, max_texture_dimension2_d, "max-texture-dimension2-d")           \
    X(maxTextureDimension3D, max_texture_dimension3_d, "max-texture-dimension3-d")           \
    X(maxTextureArrayLayers, max_texture_array_layers, "max-texture-array-layers")           \
    X(maxBindGroups, max_bind_groups, "max-bind-groups")                                     \
    X(maxBindGroupsPlusVertexBuffers, max_bind_groups_plus_vertex_buffers,                   \
      "max-bind-groups-plus-vertex-buffers")                                                 \
    X(maxBindingsPerBindGroup, max_bindings_per_bind_group, "max-bindings-per-bind-group")   \
    X(maxDynamicUniformBuffersPerPipelineLayout, max_dynamic_uniform_buffers_per_pipeline_layout, \
      "max-dynamic-uniform-buffers-per-pipeline-layout")                                     \
    X(maxDynamicStorageBuffersPerPipelineLayout, max_dynamic_storage_buffers_per_pipeline_layout, \
      "max-dynamic-storage-buffers-per-pipeline-layout")                                     \
    X(maxSampledTexturesPerShaderStage, max_sampled_textures_per_shader_stage,               \
      "max-sampled-textures-per-shader-stage")                                               \
    X(maxSamplersPerShaderStage, max_samplers_per_shader_stage, "max-samplers-per-shader-stage") \
    X(maxStorageBuffersPerShaderStage, max_storage_buffers_per_shader_stage,                 \
      "max-storage-buffers-per-shader-stage")                                                \
    X(maxStorageTexturesPerShaderStage, max_storage_textures_per_shader_stage,               \
      "max-storage-textures-per-shader-stage")                                               \
    X(maxUniformBuffersPerShaderStage, max_uniform_buffers_per_shader_stage,                 \
      "max-uniform-buffers-per-shader-stage")                                                \
    X(minUniformBufferOffsetAlignment, min_uniform_buffer_offset_alignment,                  \
      "min-uniform-buffer-offset-alignment")                                                 \
    X(minStorageBufferOffsetAlignment, min_storage_buffer_offset_alignment,                  \
      "min-storage-buffer-offset-alignment")                                                 \
    X(maxVertexBuffers, max_vertex_buffers, "max-vertex-buffers")                            \
    X(maxVertexAttributes, max_vertex_attributes, "max-vertex-attributes")                   \
    X(maxVertexBufferArrayStride, max_vertex_buffer_array_stride, "max-vertex-buffer-array-stride") \
    X(maxInterStageShaderVariables, max_inter_stage_shader_variables,                        \
      "max-inter-stage-shader-variables")                                                    \
    X(maxColorAttachments, max_color_attachments, "max-color-attachments")                   \
    X(maxColorAttachmentBytesPerSample, max_color_attachment_bytes_per_sample,               \
      "max-color-attachment-bytes-per-sample")                                               \
    X(maxComputeWorkgroupStorageSize, max_compute_workgroup_storage_size,                    \
      "max-compute-workgroup-storage-size")                                                  \
    X(maxComputeInvocationsPerWorkgroup, max_compute_invocations_per_workgroup,              \
      "max-compute-invocations-per-workgroup")                                               \
    X(maxComputeWorkgroupSizeX, max_compute_workgroup_size_x, "max-compute-workgroup-size-x") \
    X(maxComputeWorkgroupSizeY, max_compute_workgroup_size_y, "max-compute-workgroup-size-y") \
    X(maxComputeWorkgroupSizeZ, max_compute_workgroup_size_z, "max-compute-workgroup-size-z") \
    X(maxComputeWorkgroupsPerDimension, max_compute_workgroups_per_dimension,                \
      "max-compute-workgroups-per-dimension")

#define LIMITS_U64(X)                                                                        \
    X(maxUniformBufferBindingSize, max_uniform_buffer_binding_size, "max-uniform-buffer-binding-size") \
    X(maxStorageBufferBindingSize, max_storage_buffer_binding_size, "max-storage-buffer-binding-size") \
    X(maxBufferSize, max_buffer_size, "max-buffer-size")

static void limits_from_wasi(wasi_webgpu_webgpu_own_gpu_supported_limits_t handle, WGPULimits* limits) {
    wasi_webgpu_webgpu_borrow_gpu_supported_limits_t b = BORROW(gpu_supported_limits, handle);
#define GET(field, wasi, name) limits->field = wasi_webgpu_webgpu_method_gpu_supported_limits_##wasi(b);
    LIMITS_U32(GET)
    LIMITS_U64(GET)
#undef GET
    limits->maxImmediateSize = 0;
    wasi_webgpu_webgpu_gpu_supported_limits_drop_own(handle);
}

// The limits a device descriptor asks for, as the record the WIT takes, or
// none when every limit is left undefined.
static wasi_webgpu_webgpu_option_own_record_option_gpu_size64_t limits_to_wasi(WGPULimits const* limits) {
    wasi_webgpu_webgpu_option_own_record_option_gpu_size64_t out = {0};
    if (!limits) return out;
    wasi_webgpu_webgpu_own_record_option_gpu_size64_t record = {0};
    bool any = false;
#define ADD(field, wasi, name, undefined)                                                  \
    if (limits->field != undefined) {                                                      \
        if (!any) record = wasi_webgpu_webgpu_constructor_record_option_gpu_size64();      \
        any = true;                                                                        \
        webgpu_wasi_string_t key = str_lit(name);                                          \
        wasi_webgpu_webgpu_gpu_size64_t value = limits->field;                             \
        wasi_webgpu_webgpu_method_record_option_gpu_size64_add(BORROW(record_option_gpu_size64, record), &key, &value); \
    }
#define ADD32(field, wasi, name) ADD(field, wasi, name, WGPU_LIMIT_U32_UNDEFINED)
#define ADD64(field, wasi, name) ADD(field, wasi, name, WGPU_LIMIT_U64_UNDEFINED)
    LIMITS_U32(ADD32)
    LIMITS_U64(ADD64)
#undef ADD32
#undef ADD64
#undef ADD
    if (any) {
        out.is_some = true;
        out.val = record;
    }
    return out;
}

static const char* feature_name(WGPUFeatureName feature) {
    for (size_t i = 0; i < sizeof(wgpu_wasi_feature_names) / sizeof(wgpu_wasi_feature_names[0]); i++) {
        if (wgpu_wasi_feature_names[i].feature == feature) return wgpu_wasi_feature_names[i].name;
    }
    return NULL;
}

static bool features_has(wasi_webgpu_webgpu_own_gpu_supported_features_t handle, WGPUFeatureName feature) {
    const char* name = feature_name(feature);
    if (!name) return false;
    webgpu_wasi_string_t s = str_lit(name);
    return wasi_webgpu_webgpu_method_gpu_supported_features_has(BORROW(gpu_supported_features, handle), &s);
}

static void features_list(wasi_webgpu_webgpu_own_gpu_supported_features_t handle, WGPUSupportedFeatures* features) {
    size_t n = sizeof(wgpu_wasi_feature_names) / sizeof(wgpu_wasi_feature_names[0]);
    WGPUFeatureName* list = xcalloc(n, sizeof(WGPUFeatureName));
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (features_has(handle, wgpu_wasi_feature_names[i].feature)) list[count++] = wgpu_wasi_feature_names[i].feature;
    }
    features->featureCount = count;
    features->features = list;
}

void wgpuSupportedFeaturesFreeMembers(WGPUSupportedFeatures supportedFeatures) {
    free((void*)supportedFeatures.features);
}

// ---------------------------------------------------------------------------
// Adapter

static void adapter_info_from_wasi(wasi_webgpu_webgpu_own_gpu_adapter_info_t handle, WGPUAdapterInfo* info) {
    wasi_webgpu_webgpu_borrow_gpu_adapter_info_t b = BORROW(gpu_adapter_info, handle);
    webgpu_wasi_string_t s;
    wasi_webgpu_webgpu_method_gpu_adapter_info_vendor(b, &s);
    info->vendor = str_out_owned(&s);
    wasi_webgpu_webgpu_method_gpu_adapter_info_architecture(b, &s);
    info->architecture = str_out_owned(&s);
    wasi_webgpu_webgpu_method_gpu_adapter_info_device(b, &s);
    info->device = str_out_owned(&s);
    wasi_webgpu_webgpu_method_gpu_adapter_info_description(b, &s);
    info->description = str_out_owned(&s);
    info->subgroupMinSize = wasi_webgpu_webgpu_method_gpu_adapter_info_subgroup_min_size(b);
    info->subgroupMaxSize = wasi_webgpu_webgpu_method_gpu_adapter_info_subgroup_max_size(b);
    // Not in the WIT: the host does not say which backend or kind of adapter.
    info->backendType = WGPUBackendType_Undefined;
    info->adapterType = WGPUAdapterType_Unknown;
    info->vendorID = 0;
    info->deviceID = 0;
    wasi_webgpu_webgpu_gpu_adapter_info_drop_own(handle);
}

WGPUStatus wgpuAdapterGetInfo(WGPUAdapter adapter, WGPUAdapterInfo* info) {
    if (!adapter || !info) return WGPUStatus_Error;
    adapter_info_from_wasi(wasi_webgpu_webgpu_method_gpu_adapter_info(BORROW(gpu_adapter, adapter->adapter)), info);
    return WGPUStatus_Success;
}

void wgpuAdapterInfoFreeMembers(WGPUAdapterInfo adapterInfo) {
    free((void*)adapterInfo.vendor.data);
    free((void*)adapterInfo.architecture.data);
    free((void*)adapterInfo.device.data);
    free((void*)adapterInfo.description.data);
}

WGPUStatus wgpuAdapterGetLimits(WGPUAdapter adapter, WGPULimits* limits) {
    if (!adapter || !limits) return WGPUStatus_Error;
    limits_from_wasi(wasi_webgpu_webgpu_method_gpu_adapter_limits(BORROW(gpu_adapter, adapter->adapter)), limits);
    return WGPUStatus_Success;
}

WGPUBool wgpuAdapterHasFeature(WGPUAdapter adapter, WGPUFeatureName feature) {
    wasi_webgpu_webgpu_own_gpu_supported_features_t f =
        wasi_webgpu_webgpu_method_gpu_adapter_features(BORROW(gpu_adapter, adapter->adapter));
    bool has = features_has(f, feature);
    wasi_webgpu_webgpu_gpu_supported_features_drop_own(f);
    return has;
}

void wgpuAdapterGetFeatures(WGPUAdapter adapter, WGPUSupportedFeatures* features) {
    wasi_webgpu_webgpu_own_gpu_supported_features_t f =
        wasi_webgpu_webgpu_method_gpu_adapter_features(BORROW(gpu_adapter, adapter->adapter));
    features_list(f, features);
    wasi_webgpu_webgpu_gpu_supported_features_drop_own(f);
}

void wgpuAdapterAddRef(WGPUAdapter adapter) {
    if (!adapter) fatal("wgpuAdapterAddRef on NULL");
    adapter->refCount++;
}

void wgpuAdapterRelease(WGPUAdapter adapter) {
    if (!adapter) fatal("wgpuAdapterRelease on NULL");
    if (adapter->refCount == 0) fatal("wgpuAdapterRelease past zero");
    if (--adapter->refCount) return;
    wasi_webgpu_webgpu_gpu_adapter_drop_own(adapter->adapter);
    wgpuInstanceRelease(adapter->instance);
    free(adapter);
}

// ---------------------------------------------------------------------------
// Device

static void device_fire_lost(WGPUDeviceImpl* device, WGPUDeviceLostReason reason, const char* message) {
    if (device->lostFired || !device->lost.callback) return;
    device->lostFired = true;
    WGPUDevice handle = device;
    device->lost.callback(&handle, reason, str_view(message), device->lost.userdata1, device->lost.userdata2);
}

WGPUFuture wgpuAdapterRequestDevice(WGPUAdapter adapter, WGPU_NULLABLE WGPUDeviceDescriptor const* descriptor,
                                    WGPURequestDeviceCallbackInfo callbackInfo) {
    WGPUInstanceImpl* instance = adapter->instance;
    Event* e = event_new(EVENT_REQUEST_DEVICE, callbackInfo.mode, (void*)callbackInfo.callback, callbackInfo.userdata1,
                         callbackInfo.userdata2);

    wasi_webgpu_webgpu_gpu_device_descriptor_t d = {0};
    wasi_webgpu_webgpu_gpu_feature_name_t* features = NULL;
    if (descriptor) {
        d.label = label_in(descriptor->label);
        if (descriptor->requiredFeatureCount) {
            features = xcalloc(descriptor->requiredFeatureCount, sizeof(*features));
            for (size_t i = 0; i < descriptor->requiredFeatureCount; i++) {
                if (!wgpu_wasi_feature_name_to_wasi(descriptor->requiredFeatures[i], &features[i])) {
                    free(features);
                    e->status = WGPURequestDeviceStatus_Error;
                    e->message = message_copy("a required feature has no wasi:webgpu 0.0.1 name");
                    return event_complete(instance, e);
                }
            }
            d.required_features.is_some = true;
            d.required_features.val.ptr = features;
            d.required_features.val.len = descriptor->requiredFeatureCount;
        }
        d.required_limits = limits_to_wasi(descriptor->requiredLimits);
    }

    wasi_webgpu_webgpu_own_gpu_device_t handle;
    wasi_webgpu_webgpu_request_device_error_t err;
    // required-limits is an own handle: the call consumes it.
    bool ok = wasi_webgpu_webgpu_method_gpu_adapter_request_device(BORROW(gpu_adapter, adapter->adapter),
                                                                   descriptor ? &d : NULL, &handle, &err);
    free(features);
    if (!ok) {
        e->status = WGPURequestDeviceStatus_Error;
        e->message = message_from_wasi(&err.message);
        return event_complete(instance, e);
    }

    WGPUDeviceImpl* device = xcalloc(1, sizeof(WGPUDeviceImpl));
    device->refCount = 1;
    device->instance = instance;
    device->adapter = adapter;
    device->device = handle;
    wgpuAdapterAddRef(adapter);
    wgpuInstanceAddRef(instance);
    if (descriptor) {
        device->lost = descriptor->deviceLostCallbackInfo;
        device->uncaptured = descriptor->uncapturedErrorCallbackInfo;
    }
    if (device->uncaptured.callback) {
        device->errorPollable =
            wasi_webgpu_webgpu_method_gpu_device_onuncapturederror_subscribe(BORROW(gpu_device, device->device));
        device->hasErrorPollable = true;
    }
    device->nextDevice = instance->devices;
    instance->devices = device;

    e->status = WGPURequestDeviceStatus_Success;
    e->object = device;
    return event_complete(instance, e);
}

WGPUStatus wgpuDeviceGetLimits(WGPUDevice device, WGPULimits* limits) {
    if (!device || !limits) return WGPUStatus_Error;
    limits_from_wasi(wasi_webgpu_webgpu_method_gpu_device_limits(BORROW(gpu_device, device->device)), limits);
    return WGPUStatus_Success;
}

WGPUBool wgpuDeviceHasFeature(WGPUDevice device, WGPUFeatureName feature) {
    wasi_webgpu_webgpu_own_gpu_supported_features_t f =
        wasi_webgpu_webgpu_method_gpu_device_features(BORROW(gpu_device, device->device));
    bool has = features_has(f, feature);
    wasi_webgpu_webgpu_gpu_supported_features_drop_own(f);
    return has;
}

void wgpuDeviceGetFeatures(WGPUDevice device, WGPUSupportedFeatures* features) {
    wasi_webgpu_webgpu_own_gpu_supported_features_t f =
        wasi_webgpu_webgpu_method_gpu_device_features(BORROW(gpu_device, device->device));
    features_list(f, features);
    wasi_webgpu_webgpu_gpu_supported_features_drop_own(f);
}

WGPUStatus wgpuDeviceGetAdapterInfo(WGPUDevice device, WGPUAdapterInfo* info) {
    if (!device || !info) return WGPUStatus_Error;
    adapter_info_from_wasi(wasi_webgpu_webgpu_method_gpu_device_adapter_info(BORROW(gpu_device, device->device)), info);
    return WGPUStatus_Success;
}

WGPUAdapter wgpuDeviceGetAdapter(WGPUDevice device) {
    wgpuAdapterAddRef(device->adapter);
    return device->adapter;
}

WGPUQueue wgpuDeviceGetQueue(WGPUDevice device) {
    if (!device->queue) {
        WGPUQueueImpl* queue = xcalloc(1, sizeof(WGPUQueueImpl));
        queue->refCount = 1;  // the device's own reference
        queue->instance = device->instance;
        wgpuInstanceAddRef(device->instance);
        queue->queue = wasi_webgpu_webgpu_method_gpu_device_queue(BORROW(gpu_device, device->device));
        device->queue = queue;
    }
    device->queue->refCount++;
    return device->queue;
}

void wgpuDevicePushErrorScope(WGPUDevice device, WGPUErrorFilter filter) {
    wasi_webgpu_webgpu_gpu_error_filter_t f;
    if (!wgpu_wasi_error_filter_to_wasi(filter, &f)) fatal("wgpuDevicePushErrorScope: unknown filter");
    wasi_webgpu_webgpu_method_gpu_device_push_error_scope(BORROW(gpu_device, device->device), f);
}

WGPUFuture wgpuDevicePopErrorScope(WGPUDevice device, WGPUPopErrorScopeCallbackInfo callbackInfo) {
    Event* e = event_new(EVENT_POP_ERROR_SCOPE, callbackInfo.mode, (void*)callbackInfo.callback, callbackInfo.userdata1,
                         callbackInfo.userdata2);
    wasi_webgpu_webgpu_option_own_gpu_error_t error;
    wasi_webgpu_webgpu_pop_error_scope_error_t err;
    if (!wasi_webgpu_webgpu_method_gpu_device_pop_error_scope(BORROW(gpu_device, device->device), &error, &err)) {
        e->status = WGPUPopErrorScopeStatus_Error;
        e->errorType = WGPUErrorType_NoError;
        e->message = message_from_wasi(&err.message);
        return event_complete(device->instance, e);
    }
    e->status = WGPUPopErrorScopeStatus_Success;
    e->errorType = WGPUErrorType_NoError;
    if (error.is_some) {
        wasi_webgpu_webgpu_borrow_gpu_error_t b = BORROW(gpu_error, error.val);
        wasi_webgpu_webgpu_gpu_error_kind_t kind;
        wasi_webgpu_webgpu_method_gpu_error_kind(b, &kind);
        switch (kind.tag) {
        case WASI_WEBGPU_WEBGPU_GPU_ERROR_KIND_VALIDATION_ERROR: e->errorType = WGPUErrorType_Validation; break;
        case WASI_WEBGPU_WEBGPU_GPU_ERROR_KIND_OUT_OF_MEMORY_ERROR: e->errorType = WGPUErrorType_OutOfMemory; break;
        case WASI_WEBGPU_WEBGPU_GPU_ERROR_KIND_INTERNAL_ERROR: e->errorType = WGPUErrorType_Internal; break;
        default: e->errorType = WGPUErrorType_Unknown; break;
        }
        webgpu_wasi_string_t message;
        wasi_webgpu_webgpu_method_gpu_error_message(b, &message);
        e->message = message_from_wasi(&message);
        wasi_webgpu_webgpu_gpu_error_drop_own(error.val);
    }
    return event_complete(device->instance, e);
}

void wgpuDeviceDestroy(WGPUDevice device) {
    wasi_webgpu_webgpu_method_gpu_device_destroy(BORROW(gpu_device, device->device));
    device_fire_lost(device, WGPUDeviceLostReason_Destroyed, "the device was destroyed");
}

void wgpuDeviceAddRef(WGPUDevice device) {
    if (!device) fatal("wgpuDeviceAddRef on NULL");
    device->refCount++;
}

void wgpuDeviceRelease(WGPUDevice device) {
    if (!device) fatal("wgpuDeviceRelease on NULL");
    if (device->refCount == 0) fatal("wgpuDeviceRelease past zero");
    if (--device->refCount) return;
    // The last reference going is the device going: tell its lost callback
    // now, while its userdata is still the application's to touch.
    device_fire_lost(device, WGPUDeviceLostReason_Destroyed, "the device was released");
    WGPUDeviceImpl** link = &device->instance->devices;
    while (*link && *link != device) link = &(*link)->nextDevice;
    if (*link) *link = device->nextDevice;
    if (device->hasErrorPollable) webgpu_wasi_poll_pollable_drop_own(device->errorPollable);
    if (device->queue) wgpuQueueRelease(device->queue);
    wasi_webgpu_webgpu_gpu_device_drop_own(device->device);
    wgpuAdapterRelease(device->adapter);
    wgpuInstanceRelease(device->instance);
    free(device);
}

// ---------------------------------------------------------------------------
// Buffers

WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice device, WGPUBufferDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_buffer_descriptor_t d = {0};
    d.size = descriptor->size;
    d.usage = (wasi_webgpu_webgpu_gpu_buffer_usage_flags_t)descriptor->usage;
    d.label = label_in(descriptor->label);
    if (descriptor->mappedAtCreation) {
        d.mapped_at_creation.is_some = true;
        d.mapped_at_creation.val = true;
    }
    WGPUBufferImpl* buffer = xcalloc(1, sizeof(WGPUBufferImpl));
    buffer->refCount = 1;
    buffer->instance = device->instance;
    buffer->buffer = wasi_webgpu_webgpu_method_gpu_device_create_buffer(BORROW(gpu_device, device->device), &d);
    buffer->size = descriptor->size;
    buffer->usage = descriptor->usage;
    buffer->mapState = WGPUBufferMapState_Unmapped;
    wgpuInstanceAddRef(device->instance);
    if (descriptor->mappedAtCreation) {
        buffer->mapState = WGPUBufferMapState_Mapped;
        buffer->mapMode = WGPUMapMode_Write;
        buffer->mapOffset = 0;
        buffer->mapSize = descriptor->size;
        buffer->shadow = xcalloc(1, (size_t)descriptor->size);
    }
    return buffer;
}

WGPUFuture wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode mode, size_t offset, size_t size,
                              WGPUBufferMapCallbackInfo callbackInfo) {
    Event* e = event_new(EVENT_MAP, callbackInfo.mode, (void*)callbackInfo.callback, callbackInfo.userdata1,
                         callbackInfo.userdata2);
    uint64_t map_size = size == WGPU_WHOLE_MAP_SIZE ? (offset <= buffer->size ? buffer->size - offset : 0) : size;
    if (buffer->mapState != WGPUBufferMapState_Unmapped) {
        e->status = WGPUMapAsyncStatus_Error;
        e->message = message_copy("the buffer is already mapped or being mapped");
        return event_complete(buffer->instance, e);
    }
    wasi_webgpu_webgpu_gpu_size64_t o = offset;
    wasi_webgpu_webgpu_gpu_size64_t s = map_size;
    wasi_webgpu_webgpu_map_async_error_t err;
    // Blocks until the host has the range mapped.
    if (!wasi_webgpu_webgpu_method_gpu_buffer_map_async(BORROW(gpu_buffer, buffer->buffer),
                                                        (wasi_webgpu_webgpu_gpu_map_mode_flags_t)mode, &o, &s, &err)) {
        e->status = err.kind.tag == WASI_WEBGPU_WEBGPU_MAP_ASYNC_ERROR_KIND_ABORT_ERROR ? WGPUMapAsyncStatus_Aborted
                                                                                         : WGPUMapAsyncStatus_Error;
        e->message = message_from_wasi(&err.message);
        return event_complete(buffer->instance, e);
    }
    buffer->mapState = WGPUBufferMapState_Mapped;
    buffer->mapMode = mode;
    buffer->mapOffset = offset;
    buffer->mapSize = map_size;
    buffer->shadow = NULL;
    e->status = WGPUMapAsyncStatus_Success;
    return event_complete(buffer->instance, e);
}

// The guest copy of the whole mapped range, made on first use.
static uint8_t* buffer_shadow(WGPUBufferImpl* buffer) {
    if (buffer->shadow) return buffer->shadow;
    if (buffer->mapMode & WGPUMapMode_Read) {
        wasi_webgpu_webgpu_gpu_size64_t o = buffer->mapOffset;
        wasi_webgpu_webgpu_gpu_size64_t s = buffer->mapSize;
        webgpu_wasi_list_u8_t bytes;
        wasi_webgpu_webgpu_get_mapped_range_error_t err;
        if (!wasi_webgpu_webgpu_method_gpu_buffer_get_mapped_range_get_with_copy(BORROW(gpu_buffer, buffer->buffer), &o,
                                                                                &s, &bytes, &err)) {
            wasi_webgpu_webgpu_get_mapped_range_error_free(&err);
            return NULL;
        }
        if (bytes.len < buffer->mapSize) {
            webgpu_wasi_list_u8_free(&bytes);
            return NULL;
        }
        // The list was allocated with malloc (cabi_realloc), so it is kept as is.
        buffer->shadow = bytes.ptr ? bytes.ptr : xmalloc(1);
    } else {
        buffer->shadow = xcalloc(1, (size_t)buffer->mapSize);
    }
    return buffer->shadow;
}

static void* buffer_range(WGPUBuffer buffer, size_t offset, size_t size) {
    if (!buffer || buffer->mapState != WGPUBufferMapState_Mapped) return NULL;
    uint64_t end_of_map = buffer->mapOffset + buffer->mapSize;
    if (offset < buffer->mapOffset || offset > end_of_map) return NULL;
    uint64_t length = size == WGPU_WHOLE_MAP_SIZE ? end_of_map - offset : size;
    if (offset + length > end_of_map) return NULL;
    uint8_t* shadow = buffer_shadow(buffer);
    return shadow ? shadow + (offset - buffer->mapOffset) : NULL;
}

void* wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
    return buffer_range(buffer, offset, size);
}

void const* wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
    return buffer_range(buffer, offset, size);
}

WGPUStatus wgpuBufferReadMappedRange(WGPUBuffer buffer, size_t offset, void* data, size_t size) {
    const void* src = buffer_range(buffer, offset, size);
    if (!src) return WGPUStatus_Error;
    memcpy(data, src, size);
    return WGPUStatus_Success;
}

WGPUStatus wgpuBufferWriteMappedRange(WGPUBuffer buffer, size_t offset, void const* data, size_t size) {
    void* dst = buffer_range(buffer, offset, size);
    if (!dst) return WGPUStatus_Error;
    memcpy(dst, data, size);
    return WGPUStatus_Success;
}

void wgpuBufferUnmap(WGPUBuffer buffer) {
    if (buffer->mapState != WGPUBufferMapState_Mapped) return;
    if (buffer->shadow && (buffer->mapMode & WGPUMapMode_Write)) {
        webgpu_wasi_list_u8_t data = {buffer->shadow, (size_t)buffer->mapSize};
        wasi_webgpu_webgpu_gpu_size64_t o = buffer->mapOffset;
        wasi_webgpu_webgpu_gpu_size64_t s = buffer->mapSize;
        wasi_webgpu_webgpu_get_mapped_range_error_t err;
        if (!wasi_webgpu_webgpu_method_gpu_buffer_get_mapped_range_set_with_copy(BORROW(gpu_buffer, buffer->buffer),
                                                                                &data, &o, &s, &err)) {
            wasi_webgpu_webgpu_get_mapped_range_error_free(&err);
        }
    }
    free(buffer->shadow);
    buffer->shadow = NULL;
    wasi_webgpu_webgpu_unmap_error_t err;
    if (!wasi_webgpu_webgpu_method_gpu_buffer_unmap(BORROW(gpu_buffer, buffer->buffer), &err)) {
        wasi_webgpu_webgpu_unmap_error_free(&err);
    }
    buffer->mapState = WGPUBufferMapState_Unmapped;
}

WGPUBufferMapState wgpuBufferGetMapState(WGPUBuffer buffer) { return buffer->mapState; }
uint64_t wgpuBufferGetSize(WGPUBuffer buffer) { return buffer->size; }
WGPUBufferUsage wgpuBufferGetUsage(WGPUBuffer buffer) { return buffer->usage; }

void wgpuBufferDestroy(WGPUBuffer buffer) {
    free(buffer->shadow);
    buffer->shadow = NULL;
    buffer->mapState = WGPUBufferMapState_Unmapped;
    wasi_webgpu_webgpu_method_gpu_buffer_destroy(BORROW(gpu_buffer, buffer->buffer));
}

void wgpuBufferAddRef(WGPUBuffer buffer) {
    if (!buffer) fatal("wgpuBufferAddRef on NULL");
    buffer->refCount++;
}

void wgpuBufferRelease(WGPUBuffer buffer) {
    if (!buffer) fatal("wgpuBufferRelease on NULL");
    if (buffer->refCount == 0) fatal("wgpuBufferRelease past zero");
    if (--buffer->refCount) return;
    free(buffer->shadow);
    wasi_webgpu_webgpu_gpu_buffer_drop_own(buffer->buffer);
    wgpuInstanceRelease(buffer->instance);
    free(buffer);
}

// ---------------------------------------------------------------------------
// Textures, views and samplers

static wasi_webgpu_webgpu_gpu_texture_format_t texture_format(WGPUTextureFormat format) {
    wasi_webgpu_webgpu_gpu_texture_format_t f;
    if (!wgpu_wasi_texture_format_to_wasi(format, &f)) fatal("texture format has no wasi:webgpu 0.0.1 name");
    return f;
}

WGPUTexture wgpuDeviceCreateTexture(WGPUDevice device, WGPUTextureDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_texture_descriptor_t d = {0};
    d.size.width = descriptor->size.width;
    d.size.height.is_some = true;
    d.size.height.val = descriptor->size.height;
    d.size.depth_or_array_layers.is_some = true;
    d.size.depth_or_array_layers.val = descriptor->size.depthOrArrayLayers;
    d.mip_level_count.is_some = true;
    d.mip_level_count.val = descriptor->mipLevelCount;
    d.sample_count.is_some = true;
    d.sample_count.val = descriptor->sampleCount;
    if (descriptor->dimension != WGPUTextureDimension_Undefined) {
        d.dimension.is_some = wgpu_wasi_texture_dimension_to_wasi(descriptor->dimension, &d.dimension.val);
    }
    d.format = texture_format(descriptor->format);
    d.usage = (wasi_webgpu_webgpu_gpu_texture_usage_flags_t)descriptor->usage;
    d.label = label_in(descriptor->label);
    wasi_webgpu_webgpu_gpu_texture_format_t* views = NULL;
    if (descriptor->viewFormatCount) {
        views = xcalloc(descriptor->viewFormatCount, sizeof(*views));
        for (size_t i = 0; i < descriptor->viewFormatCount; i++) views[i] = texture_format(descriptor->viewFormats[i]);
        d.view_formats.is_some = true;
        d.view_formats.val.ptr = views;
        d.view_formats.val.len = descriptor->viewFormatCount;
    }
    WGPUTextureImpl* texture = xcalloc(1, sizeof(WGPUTextureImpl));
    texture->refCount = 1;
    texture->texture = wasi_webgpu_webgpu_method_gpu_device_create_texture(BORROW(gpu_device, device->device), &d);
    texture->desc = *descriptor;
    texture->desc.label = (WGPUStringView){NULL, 0};
    texture->desc.viewFormatCount = 0;
    texture->desc.viewFormats = NULL;
    texture->desc.nextInChain = NULL;
    free(views);
    return texture;
}

WGPUTextureView wgpuTextureCreateView(WGPUTexture texture, WGPU_NULLABLE WGPUTextureViewDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_texture_view_descriptor_t d = {0};
    if (descriptor) {
        if (descriptor->format != WGPUTextureFormat_Undefined) {
            d.format.is_some = true;
            d.format.val = texture_format(descriptor->format);
        }
        if (descriptor->dimension != WGPUTextureViewDimension_Undefined) {
            d.dimension.is_some = wgpu_wasi_texture_view_dimension_to_wasi(descriptor->dimension, &d.dimension.val);
        }
        if (descriptor->usage != WGPUTextureUsage_None) {
            d.usage.is_some = true;
            d.usage.val = (wasi_webgpu_webgpu_gpu_texture_usage_flags_t)descriptor->usage;
        }
        if (descriptor->aspect != WGPUTextureAspect_Undefined) {
            d.aspect.is_some = wgpu_wasi_texture_aspect_to_wasi(descriptor->aspect, &d.aspect.val);
        }
        d.base_mip_level.is_some = true;
        d.base_mip_level.val = descriptor->baseMipLevel;
        if (descriptor->mipLevelCount != WGPU_MIP_LEVEL_COUNT_UNDEFINED) {
            d.mip_level_count.is_some = true;
            d.mip_level_count.val = descriptor->mipLevelCount;
        }
        d.base_array_layer.is_some = true;
        d.base_array_layer.val = descriptor->baseArrayLayer;
        if (descriptor->arrayLayerCount != WGPU_ARRAY_LAYER_COUNT_UNDEFINED) {
            d.array_layer_count.is_some = true;
            d.array_layer_count.val = descriptor->arrayLayerCount;
        }
        d.label = label_in(descriptor->label);
    }
    WGPUTextureViewImpl* view = xcalloc(1, sizeof(WGPUTextureViewImpl));
    view->refCount = 1;
    view->handle = wasi_webgpu_webgpu_method_gpu_texture_create_view(BORROW(gpu_texture, texture->texture),
                                                                     descriptor ? &d : NULL);
    return view;
}

void wgpuTextureDestroy(WGPUTexture texture) {
    wasi_webgpu_webgpu_method_gpu_texture_destroy(BORROW(gpu_texture, texture->texture));
}

uint32_t wgpuTextureGetWidth(WGPUTexture texture) { return texture->desc.size.width; }
uint32_t wgpuTextureGetHeight(WGPUTexture texture) { return texture->desc.size.height; }
uint32_t wgpuTextureGetDepthOrArrayLayers(WGPUTexture texture) { return texture->desc.size.depthOrArrayLayers; }
uint32_t wgpuTextureGetMipLevelCount(WGPUTexture texture) { return texture->desc.mipLevelCount; }
uint32_t wgpuTextureGetSampleCount(WGPUTexture texture) { return texture->desc.sampleCount; }
WGPUTextureDimension wgpuTextureGetDimension(WGPUTexture texture) { return texture->desc.dimension; }
WGPUTextureFormat wgpuTextureGetFormat(WGPUTexture texture) { return texture->desc.format; }
WGPUTextureUsage wgpuTextureGetUsage(WGPUTexture texture) { return texture->desc.usage; }

void wgpuTextureAddRef(WGPUTexture texture) {
    if (!texture) fatal("wgpuTextureAddRef on NULL");
    texture->refCount++;
}

WGPUTexture webgpu_wasi_texture_from_handle(int32_t handle, WGPUTextureDescriptor const* descriptor) {
    WGPUTextureImpl* texture = xcalloc(1, sizeof(WGPUTextureImpl));
    texture->refCount = 1;
    texture->texture.__handle = handle;
    texture->desc = *descriptor;
    texture->desc.label = (WGPUStringView){NULL, 0};
    texture->desc.viewFormatCount = 0;
    texture->desc.viewFormats = NULL;
    texture->desc.nextInChain = NULL;
    return texture;
}

int32_t webgpu_wasi_texture_into_handle(WGPUTexture texture) {
    if (!texture) fatal("webgpu_wasi_texture_into_handle on NULL");
    if (texture->refCount != 1) fatal("webgpu_wasi_texture_into_handle on a texture with other references");
    int32_t handle = texture->texture.__handle;
    free(texture);
    return handle;
}

void wgpuTextureRelease(WGPUTexture texture) {
    if (!texture) fatal("wgpuTextureRelease on NULL");
    if (texture->refCount == 0) fatal("wgpuTextureRelease past zero");
    if (--texture->refCount) return;
    wasi_webgpu_webgpu_gpu_texture_drop_own(texture->texture);
    free(texture);
}

WGPUSampler wgpuDeviceCreateSampler(WGPUDevice device, WGPU_NULLABLE WGPUSamplerDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_sampler_descriptor_t d = {0};
    if (descriptor) {
#define OPT_ENUM(field, conv, value, undefined) \
    if ((value) != (undefined)) d.field.is_some = conv((value), &d.field.val);
        OPT_ENUM(address_mode_u, wgpu_wasi_address_mode_to_wasi, descriptor->addressModeU, WGPUAddressMode_Undefined)
        OPT_ENUM(address_mode_v, wgpu_wasi_address_mode_to_wasi, descriptor->addressModeV, WGPUAddressMode_Undefined)
        OPT_ENUM(address_mode_w, wgpu_wasi_address_mode_to_wasi, descriptor->addressModeW, WGPUAddressMode_Undefined)
        OPT_ENUM(mag_filter, wgpu_wasi_filter_mode_to_wasi, descriptor->magFilter, WGPUFilterMode_Undefined)
        OPT_ENUM(min_filter, wgpu_wasi_filter_mode_to_wasi, descriptor->minFilter, WGPUFilterMode_Undefined)
        OPT_ENUM(mipmap_filter, wgpu_wasi_mipmap_filter_mode_to_wasi, descriptor->mipmapFilter,
                 WGPUMipmapFilterMode_Undefined)
        OPT_ENUM(compare, wgpu_wasi_compare_function_to_wasi, descriptor->compare, WGPUCompareFunction_Undefined)
#undef OPT_ENUM
        d.lod_min_clamp.is_some = true;
        d.lod_min_clamp.val = descriptor->lodMinClamp;
        d.lod_max_clamp.is_some = true;
        d.lod_max_clamp.val = descriptor->lodMaxClamp;
        d.max_anisotropy.is_some = true;
        d.max_anisotropy.val = descriptor->maxAnisotropy;
        d.label = label_in(descriptor->label);
    }
    WGPUSamplerImpl* sampler = xcalloc(1, sizeof(WGPUSamplerImpl));
    sampler->refCount = 1;
    sampler->handle =
        wasi_webgpu_webgpu_method_gpu_device_create_sampler(BORROW(gpu_device, device->device), descriptor ? &d : NULL);
    return sampler;
}

// ---------------------------------------------------------------------------
// Shaders, layouts, bind groups and pipelines

WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice device, WGPUShaderModuleDescriptor const* descriptor) {
    const WGPUShaderSourceWGSL* wgsl = NULL;
    for (const WGPUChainedStruct* c = descriptor->nextInChain; c; c = c->next) {
        if (c->sType == WGPUSType_ShaderSourceWGSL) wgsl = (const WGPUShaderSourceWGSL*)c;
    }
    if (!wgsl) fatal("wgpuDeviceCreateShaderModule: only WGSL sources are supported");
    wasi_webgpu_webgpu_gpu_shader_module_descriptor_t d = {0};
    d.code = str_in(wgsl->code);
    d.label = label_in(descriptor->label);
    WGPUShaderModuleImpl* module = xcalloc(1, sizeof(WGPUShaderModuleImpl));
    module->refCount = 1;
    module->handle = wasi_webgpu_webgpu_method_gpu_device_create_shader_module(BORROW(gpu_device, device->device), &d);
    return module;
}

WGPUBindGroupLayout wgpuDeviceCreateBindGroupLayout(WGPUDevice device, WGPUBindGroupLayoutDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_bind_group_layout_entry_t* entries =
        xcalloc(descriptor->entryCount, sizeof(wasi_webgpu_webgpu_gpu_bind_group_layout_entry_t));
    for (size_t i = 0; i < descriptor->entryCount; i++) {
        const WGPUBindGroupLayoutEntry* in = &descriptor->entries[i];
        wasi_webgpu_webgpu_gpu_bind_group_layout_entry_t* out = &entries[i];
        out->binding = in->binding;
        out->visibility = (wasi_webgpu_webgpu_gpu_shader_stage_flags_t)in->visibility;
        if (in->buffer.type != WGPUBufferBindingType_BindingNotUsed) {
            out->buffer.is_some = true;
            if (in->buffer.type != WGPUBufferBindingType_Undefined) {
                out->buffer.val.type.is_some =
                    wgpu_wasi_buffer_binding_type_to_wasi(in->buffer.type, &out->buffer.val.type.val);
            }
            out->buffer.val.has_dynamic_offset.is_some = true;
            out->buffer.val.has_dynamic_offset.val = in->buffer.hasDynamicOffset;
            if (in->buffer.minBindingSize) {
                out->buffer.val.min_binding_size.is_some = true;
                out->buffer.val.min_binding_size.val = in->buffer.minBindingSize;
            }
        }
        if (in->sampler.type != WGPUSamplerBindingType_BindingNotUsed) {
            out->sampler.is_some = true;
            if (in->sampler.type != WGPUSamplerBindingType_Undefined) {
                out->sampler.val.type.is_some =
                    wgpu_wasi_sampler_binding_type_to_wasi(in->sampler.type, &out->sampler.val.type.val);
            }
        }
        if (in->texture.sampleType != WGPUTextureSampleType_BindingNotUsed) {
            out->texture.is_some = true;
            if (in->texture.sampleType != WGPUTextureSampleType_Undefined) {
                out->texture.val.sample_type.is_some =
                    wgpu_wasi_texture_sample_type_to_wasi(in->texture.sampleType, &out->texture.val.sample_type.val);
            }
            if (in->texture.viewDimension != WGPUTextureViewDimension_Undefined) {
                out->texture.val.view_dimension.is_some = wgpu_wasi_texture_view_dimension_to_wasi(
                    in->texture.viewDimension, &out->texture.val.view_dimension.val);
            }
            out->texture.val.multisampled.is_some = true;
            out->texture.val.multisampled.val = in->texture.multisampled;
        }
        if (in->storageTexture.access != WGPUStorageTextureAccess_BindingNotUsed) {
            out->storage_texture.is_some = true;
            if (in->storageTexture.access != WGPUStorageTextureAccess_Undefined) {
                out->storage_texture.val.access.is_some = wgpu_wasi_storage_texture_access_to_wasi(
                    in->storageTexture.access, &out->storage_texture.val.access.val);
            }
            out->storage_texture.val.format = texture_format(in->storageTexture.format);
            if (in->storageTexture.viewDimension != WGPUTextureViewDimension_Undefined) {
                out->storage_texture.val.view_dimension.is_some = wgpu_wasi_texture_view_dimension_to_wasi(
                    in->storageTexture.viewDimension, &out->storage_texture.val.view_dimension.val);
            }
        }
    }
    wasi_webgpu_webgpu_gpu_bind_group_layout_descriptor_t d = {0};
    d.entries.ptr = entries;
    d.entries.len = descriptor->entryCount;
    d.label = label_in(descriptor->label);
    WGPUBindGroupLayoutImpl* layout = xcalloc(1, sizeof(WGPUBindGroupLayoutImpl));
    layout->refCount = 1;
    layout->handle = wasi_webgpu_webgpu_method_gpu_device_create_bind_group_layout(BORROW(gpu_device, device->device), &d);
    free(entries);
    return layout;
}

WGPUPipelineLayout wgpuDeviceCreatePipelineLayout(WGPUDevice device, WGPUPipelineLayoutDescriptor const* descriptor) {
    wasi_webgpu_webgpu_option_borrow_gpu_bind_group_layout_t* layouts =
        xcalloc(descriptor->bindGroupLayoutCount, sizeof(*layouts));
    for (size_t i = 0; i < descriptor->bindGroupLayoutCount; i++) {
        if (descriptor->bindGroupLayouts[i]) {
            layouts[i].is_some = true;
            layouts[i].val = BORROW(gpu_bind_group_layout, descriptor->bindGroupLayouts[i]->handle);
        }
    }
    wasi_webgpu_webgpu_gpu_pipeline_layout_descriptor_t d = {0};
    d.bind_group_layouts.ptr = layouts;
    d.bind_group_layouts.len = descriptor->bindGroupLayoutCount;
    d.label = label_in(descriptor->label);
    WGPUPipelineLayoutImpl* layout = xcalloc(1, sizeof(WGPUPipelineLayoutImpl));
    layout->refCount = 1;
    layout->handle = wasi_webgpu_webgpu_method_gpu_device_create_pipeline_layout(BORROW(gpu_device, device->device), &d);
    free(layouts);
    return layout;
}

WGPUBindGroup wgpuDeviceCreateBindGroup(WGPUDevice device, WGPUBindGroupDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_bind_group_entry_t* entries = xcalloc(descriptor->entryCount, sizeof(*entries));
    for (size_t i = 0; i < descriptor->entryCount; i++) {
        const WGPUBindGroupEntry* in = &descriptor->entries[i];
        wasi_webgpu_webgpu_gpu_bind_group_entry_t* out = &entries[i];
        out->binding = in->binding;
        if (in->buffer) {
            out->resource.tag = WASI_WEBGPU_WEBGPU_GPU_BINDING_RESOURCE_GPU_BUFFER_BINDING;
            wasi_webgpu_webgpu_gpu_buffer_binding_t* b = &out->resource.val.gpu_buffer_binding;
            b->buffer = BORROW(gpu_buffer, in->buffer->buffer);
            b->offset.is_some = true;
            b->offset.val = in->offset;
            if (in->size != WGPU_WHOLE_SIZE) {
                b->size.is_some = true;
                b->size.val = in->size;
            }
        } else if (in->sampler) {
            out->resource.tag = WASI_WEBGPU_WEBGPU_GPU_BINDING_RESOURCE_GPU_SAMPLER;
            out->resource.val.gpu_sampler = BORROW(gpu_sampler, in->sampler->handle);
        } else if (in->textureView) {
            out->resource.tag = WASI_WEBGPU_WEBGPU_GPU_BINDING_RESOURCE_GPU_TEXTURE_VIEW;
            out->resource.val.gpu_texture_view = BORROW(gpu_texture_view, in->textureView->handle);
        } else {
            fatal("wgpuDeviceCreateBindGroup: an entry binds nothing");
        }
    }
    wasi_webgpu_webgpu_gpu_bind_group_descriptor_t d = {0};
    d.layout = BORROW(gpu_bind_group_layout, descriptor->layout->handle);
    d.entries.ptr = entries;
    d.entries.len = descriptor->entryCount;
    d.label = label_in(descriptor->label);
    WGPUBindGroupImpl* group = xcalloc(1, sizeof(WGPUBindGroupImpl));
    group->refCount = 1;
    group->handle = wasi_webgpu_webgpu_method_gpu_device_create_bind_group(BORROW(gpu_device, device->device), &d);
    free(entries);
    return group;
}

WGPUComputePipeline wgpuDeviceCreateComputePipeline(WGPUDevice device, WGPUComputePipelineDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_compute_pipeline_descriptor_t d = {0};
    d.compute.module = BORROW(gpu_shader_module, descriptor->compute.module->handle);
    webgpu_wasi_string_t entry = str_in(descriptor->compute.entryPoint);
    if (entry.len) {
        d.compute.entry_point.is_some = true;
        d.compute.entry_point.val = entry;
    }
    if (descriptor->compute.constantCount) {
        // An own record: the call consumes it.
        wasi_webgpu_webgpu_own_record_gpu_pipeline_constant_value_t record =
            wasi_webgpu_webgpu_constructor_record_gpu_pipeline_constant_value();
        for (size_t i = 0; i < descriptor->compute.constantCount; i++) {
            webgpu_wasi_string_t key = str_in(descriptor->compute.constants[i].key);
            wasi_webgpu_webgpu_method_record_gpu_pipeline_constant_value_add(
                BORROW(record_gpu_pipeline_constant_value, record), &key, descriptor->compute.constants[i].value);
        }
        d.compute.constants.is_some = true;
        d.compute.constants.val = record;
    }
    if (descriptor->layout) {
        d.layout.tag = WASI_WEBGPU_WEBGPU_GPU_LAYOUT_MODE_SPECIFIC;
        d.layout.val.specific = BORROW(gpu_pipeline_layout, descriptor->layout->handle);
    } else {
        d.layout.tag = WASI_WEBGPU_WEBGPU_GPU_LAYOUT_MODE_AUTO;
    }
    d.label = label_in(descriptor->label);
    WGPUComputePipelineImpl* pipeline = xcalloc(1, sizeof(WGPUComputePipelineImpl));
    pipeline->refCount = 1;
    pipeline->handle =
        wasi_webgpu_webgpu_method_gpu_device_create_compute_pipeline(BORROW(gpu_device, device->device), &d);
    return pipeline;
}

WGPUBindGroupLayout wgpuComputePipelineGetBindGroupLayout(WGPUComputePipeline pipeline, uint32_t groupIndex) {
    WGPUBindGroupLayoutImpl* layout = xcalloc(1, sizeof(WGPUBindGroupLayoutImpl));
    layout->refCount = 1;
    layout->handle = wasi_webgpu_webgpu_method_gpu_compute_pipeline_get_bind_group_layout(
        BORROW(gpu_compute_pipeline, pipeline->handle), groupIndex);
    return layout;
}

// ---------------------------------------------------------------------------
// Query sets

WGPUQuerySet wgpuDeviceCreateQuerySet(WGPUDevice device, WGPUQuerySetDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_query_set_descriptor_t d = {0};
    if (!wgpu_wasi_query_type_to_wasi(descriptor->type, &d.type)) return NULL;
    d.count = descriptor->count;
    d.label = label_in(descriptor->label);
    wasi_webgpu_webgpu_own_gpu_query_set_t handle;
    wasi_webgpu_webgpu_create_query_set_error_t err;
    if (!wasi_webgpu_webgpu_method_gpu_device_create_query_set(BORROW(gpu_device, device->device), &d, &handle, &err)) {
        wasi_webgpu_webgpu_create_query_set_error_free(&err);
        return NULL;
    }
    WGPUQuerySetImpl* set = xcalloc(1, sizeof(WGPUQuerySetImpl));
    set->refCount = 1;
    set->handle = handle;
    set->type = descriptor->type;
    set->count = descriptor->count;
    return set;
}

void wgpuQuerySetDestroy(WGPUQuerySet querySet) {
    wasi_webgpu_webgpu_method_gpu_query_set_destroy(BORROW(gpu_query_set, querySet->handle));
}

uint32_t wgpuQuerySetGetCount(WGPUQuerySet querySet) { return querySet->count; }
WGPUQueryType wgpuQuerySetGetType(WGPUQuerySet querySet) { return querySet->type; }

// ---------------------------------------------------------------------------
// Command encoding

WGPUCommandEncoder wgpuDeviceCreateCommandEncoder(WGPUDevice device,
                                                  WGPU_NULLABLE WGPUCommandEncoderDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_command_encoder_descriptor_t d = {0};
    if (descriptor) d.label = label_in(descriptor->label);
    WGPUCommandEncoderImpl* encoder = xcalloc(1, sizeof(WGPUCommandEncoderImpl));
    encoder->refCount = 1;
    encoder->handle = wasi_webgpu_webgpu_method_gpu_device_create_command_encoder(BORROW(gpu_device, device->device),
                                                                                  descriptor ? &d : NULL);
    return encoder;
}

WGPUComputePassEncoder wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder encoder,
                                                          WGPU_NULLABLE WGPUComputePassDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_compute_pass_descriptor_t d = {0};
    if (descriptor) {
        d.label = label_in(descriptor->label);
        if (descriptor->timestampWrites) {
            const WGPUPassTimestampWrites* t = descriptor->timestampWrites;
            d.timestamp_writes.is_some = true;
            d.timestamp_writes.val.query_set = BORROW(gpu_query_set, t->querySet->handle);
            if (t->beginningOfPassWriteIndex != WGPU_QUERY_SET_INDEX_UNDEFINED) {
                d.timestamp_writes.val.beginning_of_pass_write_index.is_some = true;
                d.timestamp_writes.val.beginning_of_pass_write_index.val = t->beginningOfPassWriteIndex;
            }
            if (t->endOfPassWriteIndex != WGPU_QUERY_SET_INDEX_UNDEFINED) {
                d.timestamp_writes.val.end_of_pass_write_index.is_some = true;
                d.timestamp_writes.val.end_of_pass_write_index.val = t->endOfPassWriteIndex;
            }
        }
    }
    WGPUComputePassEncoderImpl* pass = xcalloc(1, sizeof(WGPUComputePassEncoderImpl));
    pass->refCount = 1;
    pass->handle = wasi_webgpu_webgpu_method_gpu_command_encoder_begin_compute_pass(
        BORROW(gpu_command_encoder, encoder->handle), descriptor ? &d : NULL);
    return pass;
}

void wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder encoder, WGPUBuffer source, uint64_t sourceOffset,
                                          WGPUBuffer destination, uint64_t destinationOffset, uint64_t size) {
    if (size == WGPU_WHOLE_SIZE) size = source->size - sourceOffset;
    wasi_webgpu_webgpu_method_gpu_command_encoder_copy_buffer_to_buffer(
        BORROW(gpu_command_encoder, encoder->handle), BORROW(gpu_buffer, source->buffer), sourceOffset,
        BORROW(gpu_buffer, destination->buffer), destinationOffset, size);
}

static wasi_webgpu_webgpu_gpu_texel_copy_texture_info_t texel_texture(WGPUTexelCopyTextureInfo const* in) {
    wasi_webgpu_webgpu_gpu_texel_copy_texture_info_t out = {0};
    out.texture = BORROW(gpu_texture, in->texture->texture);
    out.mip_level.is_some = true;
    out.mip_level.val = in->mipLevel;
    out.origin.is_some = true;
    out.origin.val.x.is_some = true;
    out.origin.val.x.val = in->origin.x;
    out.origin.val.y.is_some = true;
    out.origin.val.y.val = in->origin.y;
    out.origin.val.z.is_some = true;
    out.origin.val.z.val = in->origin.z;
    if (in->aspect != WGPUTextureAspect_Undefined) {
        out.aspect.is_some = wgpu_wasi_texture_aspect_to_wasi(in->aspect, &out.aspect.val);
    }
    return out;
}

static wasi_webgpu_webgpu_gpu_texel_copy_buffer_layout_t texel_layout(WGPUTexelCopyBufferLayout const* in) {
    wasi_webgpu_webgpu_gpu_texel_copy_buffer_layout_t out = {0};
    out.offset.is_some = true;
    out.offset.val = in->offset;
    if (in->bytesPerRow != WGPU_COPY_STRIDE_UNDEFINED) {
        out.bytes_per_row.is_some = true;
        out.bytes_per_row.val = in->bytesPerRow;
    }
    if (in->rowsPerImage != WGPU_COPY_STRIDE_UNDEFINED) {
        out.rows_per_image.is_some = true;
        out.rows_per_image.val = in->rowsPerImage;
    }
    return out;
}

static wasi_webgpu_webgpu_gpu_extent3_d_t extent(WGPUExtent3D const* in) {
    wasi_webgpu_webgpu_gpu_extent3_d_t out = {0};
    out.width = in->width;
    out.height.is_some = true;
    out.height.val = in->height;
    out.depth_or_array_layers.is_some = true;
    out.depth_or_array_layers.val = in->depthOrArrayLayers;
    return out;
}

void wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder encoder, WGPUTexelCopyTextureInfo const* source,
                                           WGPUTexelCopyBufferInfo const* destination, WGPUExtent3D const* copySize) {
    wasi_webgpu_webgpu_gpu_texel_copy_texture_info_t src = texel_texture(source);
    wasi_webgpu_webgpu_gpu_texel_copy_buffer_layout_t layout = texel_layout(&destination->layout);
    wasi_webgpu_webgpu_gpu_texel_copy_buffer_info_t dst = {0};
    dst.buffer = BORROW(gpu_buffer, destination->buffer->buffer);
    dst.offset = layout.offset;
    dst.bytes_per_row = layout.bytes_per_row;
    dst.rows_per_image = layout.rows_per_image;
    wasi_webgpu_webgpu_gpu_extent3_d_t size = extent(copySize);
    wasi_webgpu_webgpu_method_gpu_command_encoder_copy_texture_to_buffer(BORROW(gpu_command_encoder, encoder->handle),
                                                                         &src, &dst, &size);
}

void wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder encoder, WGPUTexelCopyBufferInfo const* source,
                                           WGPUTexelCopyTextureInfo const* destination, WGPUExtent3D const* copySize) {
    wasi_webgpu_webgpu_gpu_texel_copy_buffer_layout_t layout = texel_layout(&source->layout);
    wasi_webgpu_webgpu_gpu_texel_copy_buffer_info_t src = {0};
    src.buffer = BORROW(gpu_buffer, source->buffer->buffer);
    src.offset = layout.offset;
    src.bytes_per_row = layout.bytes_per_row;
    src.rows_per_image = layout.rows_per_image;
    wasi_webgpu_webgpu_gpu_texel_copy_texture_info_t dst = texel_texture(destination);
    wasi_webgpu_webgpu_gpu_extent3_d_t size = extent(copySize);
    wasi_webgpu_webgpu_method_gpu_command_encoder_copy_buffer_to_texture(BORROW(gpu_command_encoder, encoder->handle),
                                                                         &src, &dst, &size);
}

void wgpuCommandEncoderClearBuffer(WGPUCommandEncoder encoder, WGPUBuffer buffer, uint64_t offset, uint64_t size) {
    wasi_webgpu_webgpu_gpu_size64_t o = offset;
    wasi_webgpu_webgpu_gpu_size64_t s = size;
    wasi_webgpu_webgpu_method_gpu_command_encoder_clear_buffer(BORROW(gpu_command_encoder, encoder->handle),
                                                               BORROW(gpu_buffer, buffer->buffer), &o,
                                                               size == WGPU_WHOLE_SIZE ? NULL : &s);
}

void wgpuCommandEncoderResolveQuerySet(WGPUCommandEncoder encoder, WGPUQuerySet querySet, uint32_t firstQuery,
                                       uint32_t queryCount, WGPUBuffer destination, uint64_t destinationOffset) {
    wasi_webgpu_webgpu_method_gpu_command_encoder_resolve_query_set(
        BORROW(gpu_command_encoder, encoder->handle), BORROW(gpu_query_set, querySet->handle), firstQuery, queryCount,
        BORROW(gpu_buffer, destination->buffer), destinationOffset);
}

WGPUCommandBuffer wgpuCommandEncoderFinish(WGPUCommandEncoder encoder,
                                           WGPU_NULLABLE WGPUCommandBufferDescriptor const* descriptor) {
    wasi_webgpu_webgpu_gpu_command_buffer_descriptor_t d = {0};
    if (descriptor) d.label = label_in(descriptor->label);
    WGPUCommandBufferImpl* buffer = xcalloc(1, sizeof(WGPUCommandBufferImpl));
    buffer->refCount = 1;
    buffer->handle = wasi_webgpu_webgpu_method_gpu_command_encoder_finish(BORROW(gpu_command_encoder, encoder->handle),
                                                                          descriptor ? &d : NULL);
    return buffer;
}

void wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder pass, WGPUComputePipeline pipeline) {
    wasi_webgpu_webgpu_method_gpu_compute_pass_encoder_set_pipeline(BORROW(gpu_compute_pass_encoder, pass->handle),
                                                                    BORROW(gpu_compute_pipeline, pipeline->handle));
}

void wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder pass, uint32_t groupIndex,
                                        WGPU_NULLABLE WGPUBindGroup group, size_t dynamicOffsetCount,
                                        uint32_t const* dynamicOffsets) {
    wasi_webgpu_webgpu_borrow_gpu_bind_group_t b;
    if (group) b = BORROW(gpu_bind_group, group->handle);
    wasi_webgpu_webgpu_list_gpu_buffer_dynamic_offset_t offsets = {(uint32_t*)dynamicOffsets, dynamicOffsetCount};
    wasi_webgpu_webgpu_set_bind_group_error_t err;
    if (!wasi_webgpu_webgpu_method_gpu_compute_pass_encoder_set_bind_group(
            BORROW(gpu_compute_pass_encoder, pass->handle), groupIndex, group ? &b : NULL,
            dynamicOffsetCount ? &offsets : NULL, NULL, NULL, &err)) {
        wasi_webgpu_webgpu_set_bind_group_error_free(&err);
    }
}

void wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder pass, uint32_t x, uint32_t y, uint32_t z) {
    wasi_webgpu_webgpu_method_gpu_compute_pass_encoder_dispatch_workgroups(
        BORROW(gpu_compute_pass_encoder, pass->handle), x, &y, &z);
}

void wgpuComputePassEncoderDispatchWorkgroupsIndirect(WGPUComputePassEncoder pass, WGPUBuffer indirectBuffer,
                                                      uint64_t indirectOffset) {
    wasi_webgpu_webgpu_method_gpu_compute_pass_encoder_dispatch_workgroups_indirect(
        BORROW(gpu_compute_pass_encoder, pass->handle), BORROW(gpu_buffer, indirectBuffer->buffer), indirectOffset);
}

void wgpuComputePassEncoderEnd(WGPUComputePassEncoder pass) {
    wasi_webgpu_webgpu_method_gpu_compute_pass_encoder_end(BORROW(gpu_compute_pass_encoder, pass->handle));
}

// ---------------------------------------------------------------------------
// Queue

void wgpuQueueSubmit(WGPUQueue queue, size_t commandCount, WGPUCommandBuffer const* commands) {
    wasi_webgpu_webgpu_borrow_gpu_command_buffer_t* list = xcalloc(commandCount, sizeof(*list));
    for (size_t i = 0; i < commandCount; i++) list[i] = BORROW(gpu_command_buffer, commands[i]->handle);
    wasi_webgpu_webgpu_list_borrow_gpu_command_buffer_t buffers = {list, commandCount};
    wasi_webgpu_webgpu_method_gpu_queue_submit(BORROW(gpu_queue, queue->queue), &buffers);
    free(list);
}

void wgpuQueueWriteBuffer(WGPUQueue queue, WGPUBuffer buffer, uint64_t bufferOffset, void const* data, size_t size) {
    webgpu_wasi_list_u8_t bytes = {(uint8_t*)data, size};
    wasi_webgpu_webgpu_write_buffer_error_t err;
    if (!wasi_webgpu_webgpu_method_gpu_queue_write_buffer_with_copy(BORROW(gpu_queue, queue->queue),
                                                                    BORROW(gpu_buffer, buffer->buffer), bufferOffset,
                                                                    &bytes, NULL, NULL, &err)) {
        wasi_webgpu_webgpu_write_buffer_error_free(&err);
    }
}

void wgpuQueueWriteTexture(WGPUQueue queue, WGPUTexelCopyTextureInfo const* destination, void const* data,
                           size_t dataSize, WGPUTexelCopyBufferLayout const* dataLayout, WGPUExtent3D const* writeSize) {
    wasi_webgpu_webgpu_gpu_texel_copy_texture_info_t dst = texel_texture(destination);
    webgpu_wasi_list_u8_t bytes = {(uint8_t*)data, dataSize};
    wasi_webgpu_webgpu_gpu_texel_copy_buffer_layout_t layout = texel_layout(dataLayout);
    wasi_webgpu_webgpu_gpu_extent3_d_t size = extent(writeSize);
    wasi_webgpu_webgpu_method_gpu_queue_write_texture_with_copy(BORROW(gpu_queue, queue->queue), &dst, &bytes, &layout,
                                                                &size);
}

WGPUFuture wgpuQueueOnSubmittedWorkDone(WGPUQueue queue, WGPUQueueWorkDoneCallbackInfo callbackInfo) {
    // Blocks until the host's queue has drained.
    wasi_webgpu_webgpu_method_gpu_queue_on_submitted_work_done(BORROW(gpu_queue, queue->queue));
    Event* e = event_new(EVENT_WORK_DONE, callbackInfo.mode, (void*)callbackInfo.callback, callbackInfo.userdata1,
                         callbackInfo.userdata2);
    e->status = WGPUQueueWorkDoneStatus_Success;
    return event_complete(queue->instance, e);
}

void wgpuQueueAddRef(WGPUQueue queue) {
    if (!queue) fatal("wgpuQueueAddRef on NULL");
    queue->refCount++;
}

void wgpuQueueRelease(WGPUQueue queue) {
    if (!queue) fatal("wgpuQueueRelease on NULL");
    if (queue->refCount == 0) fatal("wgpuQueueRelease past zero");
    if (--queue->refCount) return;
    wasi_webgpu_webgpu_gpu_queue_drop_own(queue->queue);
    wgpuInstanceRelease(queue->instance);
    free(queue);
}

// ---------------------------------------------------------------------------
// Labels are not kept by the host.

void wgpuBufferSetLabel(WGPUBuffer o, WGPUStringView label) { (void)o; (void)label; }
void wgpuTextureSetLabel(WGPUTexture o, WGPUStringView label) { (void)o; (void)label; }
void wgpuComputePipelineSetLabel(WGPUComputePipeline o, WGPUStringView label) { (void)o; (void)label; }
void wgpuShaderModuleSetLabel(WGPUShaderModule o, WGPUStringView label) { (void)o; (void)label; }
