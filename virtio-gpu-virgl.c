#include <stdint.h>
#include <string.h>

#include <virglrenderer.h>

#include "device.h"
#include "virtio-gpu.h"
#include "virtio.h"

__attribute__((used)) const char g_vgpu_virgl_backend_link_marker[] =
    "SEMU_VIRGL_BACKEND_LINKED";

static void vgpu_virgl_delegate_reset(virtio_gpu_state_t *vgpu)
{
    virgl_renderer_reset();
    if (g_virtio_gpu_sw_backend.reset)
        g_virtio_gpu_sw_backend.reset(vgpu);
}

#define VGPU_VIRGL_DELEGATE_CMD(name)                                         \
    static void vgpu_virgl_delegate_##name(                                   \
        virtio_gpu_state_t *vgpu, struct virtq_desc *vq_desc, uint32_t *plen) \
    {                                                                         \
        g_virtio_gpu_sw_backend.name(vgpu, vq_desc, plen);                    \
    }

VGPU_VIRGL_DELEGATE_CMD(get_display_info)
VGPU_VIRGL_DELEGATE_CMD(resource_create_2d)
VGPU_VIRGL_DELEGATE_CMD(resource_unref)
VGPU_VIRGL_DELEGATE_CMD(set_scanout)
VGPU_VIRGL_DELEGATE_CMD(resource_flush)
VGPU_VIRGL_DELEGATE_CMD(transfer_to_host_2d)
VGPU_VIRGL_DELEGATE_CMD(resource_attach_backing)
VGPU_VIRGL_DELEGATE_CMD(resource_detach_backing)
VGPU_VIRGL_DELEGATE_CMD(get_edid)
VGPU_VIRGL_DELEGATE_CMD(update_cursor)
VGPU_VIRGL_DELEGATE_CMD(move_cursor)

static uint32_t vgpu_virgl_capset_id_for_index(uint32_t capset_index)
{
    uint32_t max_version = 0;
    uint32_t max_size = 0;
    uint32_t index = 0;

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL, &max_version,
                               &max_size);
    if (max_version && max_size) {
        if (capset_index == index)
            return VIRTIO_GPU_CAPSET_VIRGL;
        index++;
    }

    max_version = 0;
    max_size = 0;
    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2, &max_version,
                               &max_size);
    if (max_version && max_size && capset_index == index)
        return VIRTIO_GPU_CAPSET_VIRGL2;

    return 0;
}

static void vgpu_virgl_cmd_get_capset_info_handler(virtio_gpu_state_t *vgpu,
                                                   struct virtq_desc *vq_desc,
                                                   uint32_t *plen)
{
    const struct virtio_gpu_get_capset_info *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_get_capset_info));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int resp_idx = virtio_gpu_get_response_desc(
        vq_desc, VIRTIO_GPU_MAX_DESC,
        sizeof(struct virtio_gpu_resp_capset_info));
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    struct virtio_gpu_resp_capset_info *response = virtio_gpu_mem_guest_to_host(
        vgpu, vq_desc[resp_idx].addr,
        sizeof(struct virtio_gpu_resp_capset_info));
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }

    response->capset_id = vgpu_virgl_capset_id_for_index(request->capset_index);
    if (response->capset_id) {
        uint32_t max_version = 0;
        uint32_t max_size = 0;

        virgl_renderer_get_cap_set(response->capset_id, &max_version,
                                   &max_size);
        response->capset_max_version = max_version;
        response->capset_max_size = max_size;
    }

    *plen = sizeof(*response);
}

static void vgpu_virgl_cmd_get_capset_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    const struct virtio_gpu_get_capset *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_get_capset));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int resp_idx = virtio_gpu_get_response_desc(
        vq_desc, VIRTIO_GPU_MAX_DESC, sizeof(struct virtio_gpu_resp_capset));
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    uint32_t max_version = 0;
    uint32_t max_size = 0;
    virgl_renderer_get_cap_set(request->capset_id, &max_version, &max_size);
    if (!max_version || !max_size || request->capset_version > max_version) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, &vq_desc[resp_idx],
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    size_t response_size = sizeof(struct virtio_gpu_resp_capset) + max_size;
    if (response_size < max_size || response_size > UINT32_MAX ||
        vq_desc[resp_idx].len < response_size) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, &vq_desc[resp_idx],
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    struct virtio_gpu_resp_capset *response = virtio_gpu_mem_guest_to_host(
        vgpu, vq_desc[resp_idx].addr, (uint32_t) response_size);
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }
    virgl_renderer_fill_caps(request->capset_id, request->capset_version,
                             response->capset_data);

    *plen = (uint32_t) response_size;
}

const struct virtio_gpu_cmd_backend g_virtio_gpu_backend = {
    .reset = vgpu_virgl_delegate_reset,
    .get_display_info = vgpu_virgl_delegate_get_display_info,
    .resource_create_2d = vgpu_virgl_delegate_resource_create_2d,
    .resource_unref = vgpu_virgl_delegate_resource_unref,
    .set_scanout = vgpu_virgl_delegate_set_scanout,
    .resource_flush = vgpu_virgl_delegate_resource_flush,
    .transfer_to_host_2d = vgpu_virgl_delegate_transfer_to_host_2d,
    .resource_attach_backing = vgpu_virgl_delegate_resource_attach_backing,
    .resource_detach_backing = vgpu_virgl_delegate_resource_detach_backing,
    .get_capset_info = vgpu_virgl_cmd_get_capset_info_handler,
    .get_capset = vgpu_virgl_cmd_get_capset_handler,
    .get_edid = vgpu_virgl_delegate_get_edid,
    .resource_assign_uuid = VIRTIO_GPU_CMD_UNDEF,
    .resource_create_blob = VIRTIO_GPU_CMD_UNDEF,
    .set_scanout_blob = VIRTIO_GPU_CMD_UNDEF,
    .ctx_create = VIRTIO_GPU_CMD_UNDEF,
    .ctx_destroy = VIRTIO_GPU_CMD_UNDEF,
    .ctx_attach_resource = VIRTIO_GPU_CMD_UNDEF,
    .ctx_detach_resource = VIRTIO_GPU_CMD_UNDEF,
    .resource_create_3d = VIRTIO_GPU_CMD_UNDEF,
    .transfer_to_host_3d = VIRTIO_GPU_CMD_UNDEF,
    .transfer_from_host_3d = VIRTIO_GPU_CMD_UNDEF,
    .submit_3d = VIRTIO_GPU_CMD_UNDEF,
    .resource_map_blob = VIRTIO_GPU_CMD_UNDEF,
    .resource_unmap_blob = VIRTIO_GPU_CMD_UNDEF,
    .update_cursor = vgpu_virgl_delegate_update_cursor,
    .move_cursor = vgpu_virgl_delegate_move_cursor,
};
