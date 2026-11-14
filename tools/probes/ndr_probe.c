/* ndr_probe.c - whether the PowerPC kernel itself swaps MIG requests with Intel byte order (NDR int_rep 1).
   Sends vm_allocate (3801) to the own task once normally and once with little-endian values and NDR 1.
   Build (PPC): gcc-4.2 -o ndr_probe ndr_probe.c */
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>

typedef struct {
    mach_msg_header_t head;
    NDR_record_t ndr;
    vm_address_t addr;
    vm_size_t size;
    int flags;
} Request;

typedef struct {
    mach_msg_header_t head;
    NDR_record_t ndr;
    kern_return_t ret;
    vm_address_t addr;
    mach_msg_trailer_t trailer;
} Reply;

static uint32_t swap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }

static void attempt(int intel)
{
    union { Request a; Reply b; } m;
    mach_port_t reply = mig_get_reply_port();
    kern_return_t r;
    memset(&m, 0, sizeof m);
    m.a.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    m.a.head.msgh_size = sizeof(Request);
    m.a.head.msgh_remote_port = mach_task_self();
    m.a.head.msgh_local_port = reply;
    m.a.head.msgh_id = 3801;
    m.a.ndr = NDR_record;
    m.a.addr = 0;
    m.a.size = 0x3000;
    m.a.flags = VM_FLAGS_ANYWHERE;
    if (intel) {
        m.a.ndr.int_rep = 1;
        m.a.size = swap(0x3000);
        m.a.flags = (int)swap(VM_FLAGS_ANYWHERE);
    }
    r = mach_msg(&m.a.head, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(Request), sizeof(Reply), reply, 0, 0);
    printf("%s: mach_msg %x, reply id %d, NDR int_rep %d, ret %08x, address %08x\n", intel ? "Intel byte order" : "normal",
           r, m.b.head.msgh_id, m.b.ndr.int_rep, (unsigned)m.b.ret, (unsigned)m.b.addr);
}

int main(void)
{
    attempt(0);
    attempt(1);
    return 0;
}
