#include "qemu/osdep.h"
#include "include/qemu-common.h"
#include "chardev/char.h"
#include "hw/qdev-core.h"
#include "qemu/option.h"
#include "ui/clipboard.h"
#include "ui/console.h"
#include "ui/input.h"

#include "qapi/qapi-types-char.h"
#include "qapi/qapi-types-ui.h"

#include "spice/vd_agent.h"

#define MSGSIZE_MAX (sizeof(VDIChunkHeader) + \
                     sizeof(VDAgentMessage) + \
                     VD_AGENT_MAX_DATA_SIZE)

struct VDAgentChardev {
    Chardev parent;

    /* config */
    bool mouse;
    bool clipboard;

    /* guest vdagent */
    uint32_t caps;
    uint8_t msgbuf[MSGSIZE_MAX];
    uint32_t msgsize;

    /* mouse */
    DeviceState mouse_dev;
    uint32_t mouse_x;
    uint32_t mouse_y;
    uint32_t mouse_btn;
    QemuInputHandlerState *mouse_hs;

    /* clipboard */
    QemuClipboardPeer cbpeer;
    QemuClipboardInfo *cbinfo[QEMU_CLIPBOARD_SELECTION__COUNT];
    uint32_t cbpending[QEMU_CLIPBOARD_SELECTION__COUNT];
};
typedef struct VDAgentChardev VDAgentChardev;

#define TYPE_CHARDEV_VDAGENT "chardev-vdagent"

DECLARE_INSTANCE_CHECKER(VDAgentChardev, VDAGENT_CHARDEV,
                         TYPE_CHARDEV_VDAGENT);

/* ------------------------------------------------------------------ */
/* names, for debug logging                                           */

static const char *cap_name[] = {
    [VD_AGENT_CAP_MOUSE_STATE]                    = "mouse-state",
    [VD_AGENT_CAP_MONITORS_CONFIG]                = "monitors-config",
    [VD_AGENT_CAP_REPLY]                          = "reply",
    [VD_AGENT_CAP_CLIPBOARD]                      = "clipboard",
    [VD_AGENT_CAP_DISPLAY_CONFIG]                 = "display-config",
    [VD_AGENT_CAP_CLIPBOARD_BY_DEMAND]            = "clipboard-by-demand",
    [VD_AGENT_CAP_CLIPBOARD_SELECTION]            = "clipboard-selection",
    [VD_AGENT_CAP_SPARSE_MONITORS_CONFIG]         = "sparse-monitors-config",
    [VD_AGENT_CAP_GUEST_LINEEND_LF]               = "guest-lineend-lf",
    [VD_AGENT_CAP_GUEST_LINEEND_CRLF]             = "guest-lineend-crlf",
    [VD_AGENT_CAP_MAX_CLIPBOARD]                  = "max-clipboard",
    [VD_AGENT_CAP_AUDIO_VOLUME_SYNC]              = "audio-volume-sync",
    [VD_AGENT_CAP_MONITORS_CONFIG_POSITION]       = "monitors-config-position",
    [VD_AGENT_CAP_FILE_XFER_DISABLED]             = "file-xfer-disabled",
    [VD_AGENT_CAP_FILE_XFER_DETAILED_ERRORS]      = "file-xfer-detailed-errors",
#if 0
    [VD_AGENT_CAP_GRAPHICS_DEVICE_INFO]           = "graphics-device-info",
    [VD_AGENT_CAP_CLIPBOARD_NO_RELEASE_ON_REGRAB] = "clipboard-no-release-on-regrab",
    [VD_AGENT_CAP_CLIPBOARD_GRAB_SERIAL]          = "clipboard-grab-serial",
#endif
};

static const char *msg_name[] = {
    [VD_AGENT_MOUSE_STATE]           = "mouse-state",
    [VD_AGENT_MONITORS_CONFIG]       = "monitors-config",
    [VD_AGENT_REPLY]                 = "reply",
    [VD_AGENT_CLIPBOARD]             = "clipboard",
    [VD_AGENT_DISPLAY_CONFIG]        = "display-config",
    [VD_AGENT_ANNOUNCE_CAPABILITIES] = "announce-capabilities",
    [VD_AGENT_CLIPBOARD_GRAB]        = "clipboard-grab",
    [VD_AGENT_CLIPBOARD_REQUEST]     = "clipboard-request",
    [VD_AGENT_CLIPBOARD_RELEASE]     = "clipboard-release",
    [VD_AGENT_FILE_XFER_START]       = "file-xfer-start",
    [VD_AGENT_FILE_XFER_STATUS]      = "file-xfer-status",
    [VD_AGENT_FILE_XFER_DATA]        = "file-xfer-data",
    [VD_AGENT_CLIENT_DISCONNECTED]   = "client-disconnected",
    [VD_AGENT_MAX_CLIPBOARD]         = "max-clipboard",
    [VD_AGENT_AUDIO_VOLUME_SYNC]     = "audio-volume-sync",
#if 0
    [VD_AGENT_GRAPHICS_DEVICE_INFO]  = "graphics-device-info",
#endif
};

static const char *sel_name[] = {
    [VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD] = "clipboard",
    [VD_AGENT_CLIPBOARD_SELECTION_PRIMARY]   = "primary",
    [VD_AGENT_CLIPBOARD_SELECTION_SECONDARY] = "secondary",
};

static const char *type_name[] = {
    [VD_AGENT_CLIPBOARD_NONE]       = "none",
    [VD_AGENT_CLIPBOARD_UTF8_TEXT]  = "text",
    [VD_AGENT_CLIPBOARD_IMAGE_PNG]  = "png",
    [VD_AGENT_CLIPBOARD_IMAGE_BMP]  = "bmp",
    [VD_AGENT_CLIPBOARD_IMAGE_TIFF] = "tiff",
    [VD_AGENT_CLIPBOARD_IMAGE_JPG]  = "jpg",
#if 0
    [VD_AGENT_CLIPBOARD_FILE_LIST]  = "files",
#endif
};

#define GET_NAME(_m, _v) \
    (((_v) < ARRAY_SIZE(_m) && (_m[_v])) ? (_m[_v]) : "???")

/* ------------------------------------------------------------------ */
/* send messages                                                      */

static void vdagent_send_buf(VDAgentChardev *vd, void *ptr, uint32_t msgsize)
{
    uint8_t *msgbuf = ptr;
    uint32_t len, pos = 0;

    while (pos < msgsize) {
        len = qemu_chr_be_can_write(CHARDEV(vd));
        if (len > msgsize - pos) {
            len = msgsize - pos;
        }
        qemu_chr_be_write(CHARDEV(vd), msgbuf + pos, len);
        pos += len;
    }
}

static void vdagent_send_msg(VDAgentChardev *vd, VDAgentMessage *msg)
{
    uint8_t *msgbuf = (void *)msg;
    uint32_t msgsize = sizeof(VDAgentMessage) + msg->size;
    VDIChunkHeader chunk;

    chunk.port = VDP_CLIENT_PORT;
    chunk.size = msgsize;
    vdagent_send_buf(vd, &chunk, sizeof(chunk));

    msg->protocol = VD_AGENT_PROTOCOL;
    vdagent_send_buf(vd, msgbuf, msgsize);
    g_free(msg);
}

static void vdagent_send_caps(VDAgentChardev *vd)
{
    VDAgentMessage *msg = g_malloc0(sizeof(VDAgentMessage) +
                                    sizeof(VDAgentAnnounceCapabilities) +
                                    sizeof(uint32_t));
    VDAgentAnnounceCapabilities *caps = (void *)msg->data;

    fprintf(stderr, "%s:\n", __func__);

    msg->type = VD_AGENT_ANNOUNCE_CAPABILITIES;
    msg->size = sizeof(VDAgentAnnounceCapabilities) + sizeof(uint32_t);
    if (vd->mouse) {
        caps->caps[0] |= (1 << VD_AGENT_CAP_MOUSE_STATE);
    }
    if (vd->clipboard) {
        caps->caps[0] |= (1 << VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
        caps->caps[0] |= (1 << VD_AGENT_CAP_CLIPBOARD_SELECTION);
    }

    vdagent_send_msg(vd, msg);
}

static void vdagent_send_mouse(VDAgentChardev *vd)
{
    VDAgentMessage *msg = g_malloc0(sizeof(VDAgentMessage) +
                                    sizeof(VDAgentMouseState));
    VDAgentMouseState *mouse = (void *)msg->data;

    msg->type = VD_AGENT_MOUSE_STATE;
    msg->size = sizeof(VDAgentMouseState);

    mouse->x       = vd->mouse_x;
    mouse->y       = vd->mouse_y;
    mouse->buttons = vd->mouse_btn;

    vdagent_send_msg(vd, msg);
}

/* ------------------------------------------------------------------ */
/* mouse events                                                       */

static void vdagent_pointer_event(DeviceState *dev, QemuConsole *src,
                                  InputEvent *evt)
{
    static const int bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]        = VD_AGENT_LBUTTON_MASK,
        [INPUT_BUTTON_RIGHT]       = VD_AGENT_RBUTTON_MASK,
        [INPUT_BUTTON_MIDDLE]      = VD_AGENT_MBUTTON_MASK,
        [INPUT_BUTTON_WHEEL_UP]    = VD_AGENT_UBUTTON_MASK,
        [INPUT_BUTTON_WHEEL_DOWN]  = VD_AGENT_DBUTTON_MASK,
#if 0
        [INPUT_BUTTON_SIDE]        = VD_AGENT_SBUTTON_MASK,
        [INPUT_BUTTON_EXTRA]       = VD_AGENT_EBUTTON_MASK,
#endif
    };

    VDAgentChardev *vd = container_of(dev, struct VDAgentChardev, mouse_dev);
    InputMoveEvent *move;
    InputBtnEvent *btn;
    uint32_t xres, yres;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        xres = qemu_console_get_width(src, 1024);
        yres = qemu_console_get_height(src, 768);
        if (move->axis == INPUT_AXIS_X) {
            vd->mouse_x = qemu_input_scale_axis(move->value,
                                                INPUT_EVENT_ABS_MIN,
                                                INPUT_EVENT_ABS_MAX,
                                                0, xres);
        } else if (move->axis == INPUT_AXIS_Y) {
            vd->mouse_y = qemu_input_scale_axis(move->value,
                                                INPUT_EVENT_ABS_MIN,
                                                INPUT_EVENT_ABS_MAX,
                                                0, yres);
        }
        break;

    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (btn->down) {
            vd->mouse_btn |= bmap[btn->button];
        } else {
            vd->mouse_btn &= ~bmap[btn->button];
        }
        break;

    default:
        /* keep gcc happy */
        break;
    }
}

static void vdagent_pointer_sync(DeviceState *dev)
{
    VDAgentChardev *vd = container_of(dev, struct VDAgentChardev, mouse_dev);

    if (vd->caps & (1 << VD_AGENT_CAP_MOUSE_STATE)) {
        vdagent_send_mouse(vd);
    }
}

static QemuInputHandler vdagent_mouse_handler = {
    .name  = "vdagent mouse",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = vdagent_pointer_event,
    .sync  = vdagent_pointer_sync,
};

/* ------------------------------------------------------------------ */
/* clipboard                                                          */

static uint32_t type_qemu_to_vdagent(enum QemuClipboardType type)
{
    switch (type) {
    case QEMU_CLIPBOARD_TYPE_TEXT:
        return VD_AGENT_CLIPBOARD_UTF8_TEXT;
    default:
        return VD_AGENT_CLIPBOARD_NONE;
    }
}

static void vdagent_send_clipboard_grab(VDAgentChardev *vd,
                                        QemuClipboardInfo *info)
{
    VDAgentMessage *msg = g_malloc0(sizeof(VDAgentMessage) +
                                    sizeof(uint32_t) * (QEMU_CLIPBOARD_TYPE__COUNT + 1));
    uint8_t *s = msg->data;
    uint32_t *data = (uint32_t *)(msg->data + 4);
    uint32_t q, v, type;

    fprintf(stderr, "%s: %p\n", __func__, info);

    for (q = 0, v = 0; q < QEMU_CLIPBOARD_TYPE__COUNT; q++) {
        type = type_qemu_to_vdagent(q);
        if (type != VD_AGENT_CLIPBOARD_NONE && info->types[q].available) {
            data[v++] = type;
        }
    }

    *s = info->selection;
    msg->type = VD_AGENT_CLIPBOARD_GRAB;
    msg->size = sizeof(uint32_t) * (v + 1);

    vdagent_send_msg(vd, msg);
}

static void vdagent_send_clipboard_data(VDAgentChardev *vd,
                                        QemuClipboardInfo *info,
                                        QemuClipboardType type)
{
    VDAgentMessage *msg = g_malloc0(sizeof(VDAgentMessage) +
                                    sizeof(uint32_t) * 2 +
                                    info->types[type].size);

    uint8_t *s = msg->data;
    uint32_t *t = (uint32_t *)(msg->data + 4);
    uint8_t *d = msg->data + 8;

    fprintf(stderr, "%s: %p\n", __func__, info);

    *s = info->selection;
    *t = type_qemu_to_vdagent(type);
    memcpy(d, info->types[type].data, info->types[type].size);

    msg->type = VD_AGENT_CLIPBOARD;
    msg->size = sizeof(uint32_t) * 2 + info->types[type].size;

    vdagent_send_msg(vd, msg);
}

static void vdagent_clipboard_notify(Notifier *notifier, void *data)
{
    VDAgentChardev *vd = container_of(notifier, VDAgentChardev, cbpeer.update);
    QemuClipboardInfo *info = data;
    QemuClipboardSelection s = info->selection;
    QemuClipboardType type;
    bool self_update = info->owner == &vd->cbpeer;

    if (info != vd->cbinfo[s]) {
        fprintf(stderr, "%s: new: %p (%s)\n", __func__, info,
                info->owner ? info->owner->name : "???");
        qemu_clipboard_info_put(vd->cbinfo[s]);
        vd->cbinfo[s] = qemu_clipboard_info_get(info);
        vd->cbpending[s] = 0;
        if (!self_update) {
            fprintf(stderr, "%s: grab\n", __func__);
            vdagent_send_clipboard_grab(vd, info);
        }
        return;
    }

    if (self_update) {
        fprintf(stderr, "%s: self-update: %p (ignoring)\n", __func__, info);
        return;
    }

    fprintf(stderr, "%s: update: %p\n", __func__, info);
    for (type = 0; type < QEMU_CLIPBOARD_TYPE__COUNT; type++) {
        if (vd->cbpending[s] & (1 << type)) {
            vd->cbpending[s] &= ~(1 << type);
            vdagent_send_clipboard_data(vd, info, type);
        }
    }
}

static void vdagent_clipboard_request(QemuClipboardInfo *info,
                                      QemuClipboardType qtype)
{
    VDAgentChardev *vd = container_of(info->owner, VDAgentChardev, cbpeer);
    VDAgentMessage *msg = g_malloc0(sizeof(VDAgentMessage) +
                                    sizeof(uint32_t) * 2);
    uint32_t type = type_qemu_to_vdagent(qtype);
    uint8_t *s = msg->data;
    uint32_t *data = (uint32_t *)(msg->data + 4);

    fprintf(stderr, "%s: %p\n", __func__, info);

    if (type == VD_AGENT_CLIPBOARD_NONE) {
        return;
    }

    *s = info->selection;
    *data = type;
    msg->type = VD_AGENT_CLIPBOARD_REQUEST;
    msg->size = sizeof(uint32_t) * 2;

    vdagent_send_msg(vd, msg);
}

static void vdagent_chr_recv_clipboard(VDAgentChardev *vd, VDAgentMessage *msg)
{
    uint8_t s = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    uint32_t size = msg->size;
    void *data = msg->data;
    QemuClipboardInfo *info;
    QemuClipboardType type;

    if (vd->caps & (1 << VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        s = *(uint8_t *)data;
        data += 4;
        size -= 4;
    }
    fprintf(stderr, "    selection: %s\n", GET_NAME(sel_name, s));

    switch (msg->type) {
    case VD_AGENT_CLIPBOARD_GRAB:
        info = qemu_clipboard_info_new(&vd->cbpeer, s);
        while (size) {
            fprintf(stderr, "    grab type %s\n",
                    GET_NAME(type_name, *(uint32_t *)data));
            switch (*(uint32_t *)data) {
            case VD_AGENT_CLIPBOARD_UTF8_TEXT:
                info->types[QEMU_CLIPBOARD_TYPE_TEXT].available = true;
                break;
            default:
                break;
            }
            data += 4;
            size -= 4;
        }
        qemu_clipboard_update(info);
        qemu_clipboard_info_put(info);
        break;
    case VD_AGENT_CLIPBOARD_REQUEST:
        fprintf(stderr, "    request type %s\n",
                GET_NAME(type_name, *(uint32_t *)data));
        switch (*(uint32_t *)data) {
        case VD_AGENT_CLIPBOARD_UTF8_TEXT:
            type = QEMU_CLIPBOARD_TYPE_TEXT;
            break;
        default:
            return;
        }
        if (vd->cbinfo[s] &&
            vd->cbinfo[s]->types[type].available &&
            vd->cbinfo[s]->owner != &vd->cbpeer) {
            if (vd->cbinfo[s]->types[type].data) {
                vdagent_send_clipboard_data(vd, vd->cbinfo[s], type);
            } else {
                vd->cbpending[s] |= (1 << type);
                qemu_clipboard_request(vd->cbinfo[s], type);
            }
        }
        break;
    case VD_AGENT_CLIPBOARD: /* data */
        switch (*(uint32_t *)data) {
        case VD_AGENT_CLIPBOARD_UTF8_TEXT:
            type = QEMU_CLIPBOARD_TYPE_TEXT;
            break;
        default:
            return;
        }
        data += 4;
        size -= 4;
        fprintf(stderr, "    data: type %d, size %d\n", type, size);
        qemu_clipboard_set_data(&vd->cbpeer, vd->cbinfo[s], type,
                                size, data, true);
        break;
    case VD_AGENT_CLIPBOARD_RELEASE: /* data */
        fprintf(stderr, "    release\n");
        if (vd->cbinfo[s] &&
            vd->cbinfo[s]->owner == &vd->cbpeer) {
            /* set empty clipboard info */
            info = qemu_clipboard_info_new(NULL, s);
            qemu_clipboard_update(info);
            qemu_clipboard_info_put(info);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* chardev backend                                                    */

static void vdagent_chr_open(Chardev *chr,
                             ChardevBackend *backend,
                             bool *be_opened,
                             Error **errp)
{
    VDAgentChardev *vd = VDAGENT_CHARDEV(chr);
    ChardevVDAgent *cfg = backend->u.vdagent.data;

    vd->mouse     = cfg->has_mouse && cfg->mouse;
    vd->clipboard = cfg->has_clipboard && cfg->clipboard;

    fprintf(stderr, "%s: mouse %s, clipboard %s\n", __func__,
            vd->mouse     ? "on" : "off",
            vd->clipboard ? "on" : "off");

    if (vd->mouse) {
        vd->mouse_hs = qemu_input_handler_register(&vd->mouse_dev,
                                                   &vdagent_mouse_handler);
    }

    *be_opened = true;
}

static void vdagent_chr_recv_caps(VDAgentChardev *vd, VDAgentMessage *msg)
{
    VDAgentAnnounceCapabilities *caps = (void *)msg->data;
    int i;

    fprintf(stderr, "%s: caps.0 0x%x\n", __func__, caps->caps[0]);
    for (i = 0; i < ARRAY_SIZE(cap_name); i++) {
        if (caps->caps[0] & (1 << i)) {
            fprintf(stderr, "    cap: %s\n", cap_name[i]);
        }
    }

    vd->caps = caps->caps[0];
    if (caps->request) {
        vdagent_send_caps(vd);
    }
    if (vd->caps & (1 << VD_AGENT_CAP_MOUSE_STATE) && vd->mouse_hs) {
        qemu_input_handler_activate(vd->mouse_hs);
    }
    if (vd->caps & (1 << VD_AGENT_CAP_CLIPBOARD_BY_DEMAND) &&
        vd->caps & (1 << VD_AGENT_CAP_CLIPBOARD_SELECTION) &&
        vd->clipboard &&
        vd->cbpeer.update.notify == NULL) {
        vd->cbpeer.name = "vdagent";
        vd->cbpeer.update.notify = vdagent_clipboard_notify;
        vd->cbpeer.request = vdagent_clipboard_request;
        qemu_clipboard_peer_register(&vd->cbpeer);
    }
}

static uint32_t vdagent_chr_recv(VDAgentChardev *vd)
{
    VDIChunkHeader *chunk = (void *)vd->msgbuf;
    VDAgentMessage *msg = (void *)vd->msgbuf + sizeof(VDIChunkHeader);

    if (sizeof(VDIChunkHeader) + chunk->size > vd->msgsize) {
        fprintf(stderr, "%s: incomplete: %d/%zd\n", __func__,
                vd->msgsize, sizeof(VDIChunkHeader) + chunk->size);
        return 0;
    }

    fprintf(stderr, "%s: proto %d, type %d (%s), size %d (%zd)\n",
            __func__, msg->protocol,
            msg->type, GET_NAME(msg_name, msg->type),
            msg->size, sizeof(VDAgentMessage) + msg->size);

    switch (msg->type) {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        vdagent_chr_recv_caps(vd, msg);
        break;
    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD_RELEASE:
        vdagent_chr_recv_clipboard(vd, msg);
        break;
    default:
        fprintf(stderr, "%s: unhandled message\n", __func__);
        break;
    }

    return sizeof(VDIChunkHeader) + chunk->size;
}

static int vdagent_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    VDAgentChardev *vd = VDAGENT_CHARDEV(chr);
    uint32_t copy, move;

    copy = MSGSIZE_MAX - vd->msgsize;
    if (copy > len) {
        copy = len;
    }

    fprintf(stderr, "%s: copy %d/%d\n", __func__, copy, len);
    memcpy(vd->msgbuf + vd->msgsize, buf, copy);
    vd->msgsize += copy;

    while (vd->msgsize > sizeof(VDIChunkHeader)) {
        move = vdagent_chr_recv(vd);
        if (move == 0) {
            break;
        }

        fprintf(stderr, "%s: done %d/%d\n", __func__, move, vd->msgsize);
        memmove(vd->msgbuf, vd->msgbuf + move, vd->msgsize - move);
        vd->msgsize -= move;
    }

    return copy;
}

static void vdagent_chr_set_fe_open(struct Chardev *chr, int fe_open)
{
    VDAgentChardev *vd = VDAGENT_CHARDEV(chr);

    if (!fe_open) {
        fprintf(stderr, "%s: close\n", __func__);
        /* reset state */
        vd->msgsize = 0;
        vd->caps = 0;
        if (vd->mouse_hs) {
            qemu_input_handler_deactivate(vd->mouse_hs);
        }
        if (vd->cbpeer.update.notify) {
            qemu_clipboard_peer_unregister(&vd->cbpeer);
            memset(&vd->cbpeer, 0, sizeof(vd->cbpeer));
        }
        return;
    }

    fprintf(stderr, "%s: open\n", __func__);
}

static void vdagent_chr_parse(QemuOpts *opts, ChardevBackend *backend,
                              Error **errp)
{
    ChardevVDAgent *cfg;

    backend->type = CHARDEV_BACKEND_KIND_VDAGENT;
    cfg = backend->u.vdagent.data = g_new0(ChardevVDAgent, 1);
    qemu_chr_parse_common(opts, qapi_ChardevVDAgent_base(cfg));
    cfg->has_mouse = true;
    cfg->mouse = qemu_opt_get_bool(opts, "mouse", false);
    cfg->has_clipboard = true;
    cfg->clipboard = qemu_opt_get_bool(opts, "clipboard", false);
}

/* ------------------------------------------------------------------ */

static void vdagent_chr_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->parse            = vdagent_chr_parse;
    cc->open             = vdagent_chr_open;
    cc->chr_write        = vdagent_chr_write;
    cc->chr_set_fe_open  = vdagent_chr_set_fe_open;
}

static const TypeInfo vdagent_chr_type_info = {
    .name = TYPE_CHARDEV_VDAGENT,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(VDAgentChardev),
    .class_init = vdagent_chr_class_init,
};

static void register_types(void)
{
    type_register_static(&vdagent_chr_type_info);
}

type_init(register_types);
