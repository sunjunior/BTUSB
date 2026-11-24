/* btscan.c - 用 HCI user channel 直接跟控制器对话（不依赖 HCIDEVUP）
 * 用途：Skyworth Q6011 + btusb.ko 上验证 CSR8510 能否扫描到设备
 * 编译：aarch64-linux-gnu-gcc -static -O2 -o btscan btscan.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/rfkill.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* 把蓝牙 rfkill 全部解开（厂商的 sunxi-bt 默认 soft=1） */
static void unblock_bt_rfkill(void)
{
    int fd = open("/dev/rfkill", O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/rfkill"); return; }
    struct rfkill_event ev;
    int n;
    while ((n = read(fd, &ev, sizeof(ev))) == sizeof(ev)) {
        printf("rfkill idx=%d type=%d soft=%d hard=%d\n", ev.idx, ev.type, ev.soft, ev.hard);
        if (ev.type == RFKILL_TYPE_BLUETOOTH && (ev.soft || ev.hard)) {
            struct rfkill_event w;
            memset(&w, 0, sizeof(w));
            w.idx = ev.idx;
            w.type = ev.type;
            w.op = RFKILL_OP_CHANGE;
            w.soft = 0;
            w.hard = 0;
            if (write(fd, &w, sizeof(w)) == sizeof(w))
                printf("  -> unblocked idx=%d\n", ev.idx);
            else
                perror("  rfkill write");
        }
    }
    close(fd);
}

static int send_cmd(int fd, uint16_t opcode, const unsigned char *params, int plen)
{
    unsigned char buf[260];
    buf[0] = 0x01;                       /* HCI_COMMAND_PKT */
    buf[1] = opcode & 0xff;
    buf[2] = opcode >> 8;
    buf[3] = (unsigned char)plen;
    if (plen && params) memcpy(buf + 4, params, plen);
    return write(fd, buf, 4 + plen);
}

static int wait_event(int fd, unsigned char *evt, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) return -1;
    return read(fd, evt, 260);
}

static void print_bd(const unsigned char *b)
{
    printf("%02X:%02X:%02X:%02X:%02X:%02X", b[5], b[4], b[3], b[2], b[1], b[0]);
}

int main(int argc, char **argv)
{
    int dev = (argc > 1) ? atoi(argv[1]) : 0;
    int secs = (argc > 2) ? atoi(argv[2]) : 12;

    unblock_bt_rfkill();

    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_hci sa;
    memset(&sa, 0, sizeof(sa));
    sa.hci_family = AF_BLUETOOTH;
    sa.hci_dev = dev;
    sa.hci_channel = HCI_CHANNEL_USER;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind(user channel)");
        close(fd);
        return 1;
    }
    printf("bound hci%d (user channel)\n", dev);

    unsigned char evt[300];
    int n;

    /* Reset */
    send_cmd(fd, 0x0c03, NULL, 0);
    while ((n = wait_event(fd, evt, 2000)) > 0) {
        if (evt[0] == 0x04 && evt[1] == 0x0e) { printf("reset ok\n"); break; }
    }

    /* Read BD_ADDR */
    send_cmd(fd, 0x1009, NULL, 0);
    while ((n = wait_event(fd, evt, 2000)) > 0) {
        if (evt[0] == 0x04 && evt[1] == 0x0e && evt[4] == 0x09 && evt[5] == 0x10) {
            printf("bd_addr: ");
            print_bd(evt + 7);
            printf("\n");
            break;
        }
    }

    /* Inquiry: LAP GIAC, duration, unlimited responses */
    unsigned char p[5] = { 0x33, 0x8b, 0x9e, (unsigned char)(secs * 100 / 128), 0x00 };
    send_cmd(fd, 0x0401, p, 5);
    printf("inquiry started (%d s)...\n", secs);

    time_t end = time(NULL) + secs + 2;
    unsigned char found[32][6];
    int nfound = 0;

    while (time(NULL) < end) {
        n = wait_event(fd, evt, 500);
        if (n <= 0) continue;
        if (evt[0] != 0x04) continue;
        if (evt[1] == 0x02) {  /* Inquiry Result */
            int num = evt[3];
            int off = 4;
            for (int i = 0; i < num; i++) {
                unsigned char *b = evt + off;
                if (nfound < 32) {
                    memcpy(found[nfound], b, 6);
                    int dup = 0;
                    for (int k = 0; k < nfound; k++)
                        if (!memcmp(found[k], b, 6)) dup = 1;
                    if (!dup) {
                        nfound++;
                        printf("  found: ");
                        print_bd(b);
                        printf("  class=%02x%02x%02x\n", b[9], b[8], b[7]);
                        fflush(stdout);
                    }
                }
                off += 6 + 1 + 1 + 1 + 3 + 2;
            }
        } else if (evt[1] == 0x01) {  /* Inquiry Complete */
            printf("inquiry complete, %d device(s)\n", nfound);
            break;
        }
    }

    /* 对每个设备读名字 */
    for (int i = 0; i < nfound; i++) {
        unsigned char rp[10];
        memcpy(rp, found[i], 6);
        rp[6] = 0x00; rp[7] = 0x00; rp[8] = 0x00; rp[9] = 0x00;
        send_cmd(fd, 0x0419, rp, 10);
        while ((n = wait_event(fd, evt, 3000)) > 0) {
            if (evt[0] == 0x04 && evt[1] == 0x07) {
                if (evt[3] == 0x00) {
                    printf("  name: ");
                    print_bd(evt + 4);
                    printf(" = %s\n", (char *)(evt + 10));
                } else {
                    printf("  name: ");
                    print_bd(evt + 4);
                    printf(" = <status %d>\n", evt[3]);
                }
                break;
            }
        }
    }

    close(fd);
    return 0;
}
