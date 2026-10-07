// SPDX-License-Identifier: GPL-3.0-only
/*
 * YDP02X 触摸注入工具（AGENTS.md「Touch injection」一节的可执行实现）。
 *
 * 内核 4.4 没有 UI_DEV_SETUP / UI_ABS_SETUP，只能走 legacy uinput_user_dev 路径；
 * 设备需要是「绝对坐标指针」（EV_ABS ABS_X/ABS_Y + BTN_LEFT，不要 INPUT_PROP_DIRECT），
 * MT-B 触摸事件在 Weston 下到不了 Qt 客户端。
 *
 * 坐标：UI 逻辑点 (ux, uy)，ux∈[0,319] uy∈[0,169]
 *   raw_x = uy, raw_y = 319 - ux
 * 左手模式（应用自己把内容旋转 180°）时用 (319-ux, 169-uy) —— 用 --lefthand。
 *
 * 用法：
 *   injector tap  ui <ux> <uy> [--lefthand]
 *   injector swipe ui <x1> <y1> <x2> <y2> [--lefthand]
 *   injector longtap ui <ux> <uy> [--lefthand]      # 600ms 长按
 *   injector hold ui <ux> <uy> <ms> [--lefthand]    # 按住指定毫秒（可在此期间截图）
 *   injector press ui <ux> <uy> [--lefthand]        # 按下不松（配合外部脚本）
 *   injector release                                # 松开
 */

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int g_fd = -1;

static void emit(int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(g_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        perror("write");
    }
}

static void move_raw(int raw_x, int raw_y) {
    emit(EV_ABS, ABS_X, raw_x);
    emit(EV_ABS, ABS_Y, raw_y);
    emit(EV_SYN, SYN_REPORT, 0);
}

static void button(int down) {
    emit(EV_KEY, BTN_LEFT, down);
    emit(EV_SYN, SYN_REPORT, 0);
}

static void convert(int ux, int uy, int lefthand, int* raw_x, int* raw_y) {
    if (lefthand) {
        ux = 319 - ux;
        uy = 169 - uy;
    }
    *raw_x = uy;
    *raw_y = 319 - ux;
}

static int open_device(void) {
    g_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }
    ioctl(g_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(g_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(g_fd, UI_SET_ABSBIT, ABS_X);
    ioctl(g_fd, UI_SET_ABSBIT, ABS_Y);

    struct uinput_user_dev udev;
    memset(&udev, 0, sizeof(udev));
    snprintf(udev.name, UINPUT_MAX_NAME_SIZE, "penmods-injector");
    udev.id.bustype = BUS_USB;
    udev.id.vendor  = 0x1234;
    udev.id.product = 0x5678;
    udev.id.version = 1;
    udev.absmin[ABS_X] = 0;
    udev.absmax[ABS_X] = 170;
    udev.absmin[ABS_Y] = 0;
    udev.absmax[ABS_Y] = 320;
    if (write(g_fd, &udev, sizeof(udev)) != (ssize_t)sizeof(udev)) {
        perror("write uinput_user_dev");
        return -1;
    }
    if (ioctl(g_fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        return -1;
    }
    usleep(300000); // 等 Weston 打开设备
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s tap|longtap|swipe ui args... [--lefthand]\n", argv[0]);
        return 2;
    }
    const char* cmd = argv[1];
    int         lefthand = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--lefthand") == 0) lefthand = 1;
    }

    int ux1, uy1, ux2, uy2;
    int is_swipe   = strcmp(cmd, "swipe") == 0;
    int is_release = strcmp(cmd, "release") == 0;
    int hold_ms    = 0;
    if (strcmp(cmd, "hold") == 0) hold_ms = atoi(argv[5]);
    if (is_swipe) {
        if (argc < 7) {
            fprintf(stderr, "swipe needs 4 coords\n");
            return 2;
        }
        ux1 = atoi(argv[3]);
        uy1 = atoi(argv[4]);
        ux2 = atoi(argv[5]);
        uy2 = atoi(argv[6]);
    } else {
        ux1 = atoi(argv[3]);
        uy1 = atoi(argv[4]);
        ux2 = ux1;
        uy2 = uy1;
    }

    if (open_device() < 0) return 1;

    if (is_release) {
        button(0);
        usleep(60000);
        ioctl(g_fd, UI_DEV_DESTROY);
        close(g_fd);
        return 0;
    }

    int rx, ry, rx2, ry2;
    convert(ux1, uy1, lefthand, &rx, &ry);
    convert(ux2, uy2, lefthand, &rx2, &ry2);

    move_raw(rx, ry);
    usleep(50000);
    button(1);

    if (is_swipe) {
        const int steps = 24;
        for (int i = 1; i <= steps; ++i) {
            int x = rx + (rx2 - rx) * i / steps;
            int y = ry + (ry2 - ry) * i / steps;
            move_raw(x, y);
            usleep(12000);
        }
    } else if (strcmp(cmd, "hold") == 0) {
        usleep((useconds_t)hold_ms * 1000);
    } else if (strcmp(cmd, "press") == 0) {
        usleep(60000);
        ioctl(g_fd, UI_DEV_DESTROY);
        close(g_fd);
        return 0; // 保持按下：这里其实已经松开设备，见 README 的说明
    } else if (strcmp(cmd, "longtap") == 0) {
        usleep(600000);
    } else {
        usleep(60000);
    }

    button(0);
    usleep(60000);

    ioctl(g_fd, UI_DEV_DESTROY);
    close(g_fd);
    return 0;
}
