#include <stdlib.h>
#include <stdio.h>
extern "C" {
#include <talloc.h>
}
#include <algorithm>
#include "bufrw.h"

static void uv_test()
{
    const int N = 5*1048576;
    int *buf = (int *)malloc(sizeof(int) * N);

    printf("gen\n");
    for(int i=0; i<N/5; i++)
        buf[i] = rand() & 0x7F;
    for(int i=0; i<N/5; i++)
        buf[i] = rand() & 0x3FFF;
    for(int i=0; i<N/5; i++)
        buf[i] = rand() & 0x1FFFFF;
    for(int i=0; i<N/5; i++)
        buf[i] = rand() & 0x0FFFFFFF;
    for(int i=0; i<N/5; i++)
        buf[i] = rand() & 0xFFFFFFFF;

    printf("shuffle\n");
    std::random_shuffle(buf, buf+N);

    printf("write\n");
    Writer wr;
    for(int i=0; i<N; i++)
        wr.write_uv(buf[i]);

    printf("read\n");
    Reader rd(wr.buffer(), wr.tell());
    for(int i=0; i<N; i++)
        assert(rd.read_uv() == buf[i]);

    talloc_free(wr.buffer());
    free(buf);
}

static void utf8_test()
{
    const int N = 6*1048576;
    int *buf = (int *)malloc(sizeof(int) * N);

    printf("gen\n");
    for(int i=0; i<N/6; i++)
        buf[i] = rand() & 0x7F;
    for(int i=0; i<N/6; i++)
        buf[i] = rand() & 0x07FF;
    for(int i=0; i<N/6; i++)
        buf[i] = rand() & 0xFFFF;
    for(int i=0; i<N/6; i++)
        buf[i] = rand() & 0x1FFFFF;
    for(int i=0; i<N/6; i++)
        buf[i] = rand() & 0x3FFFFFF;
    for(int i=0; i<N/6; i++)
        buf[i] = rand() & 0x7FFFFFFF;

    printf("shuffle\n");
    std::random_shuffle(buf, buf+N);

    printf("write\n");
    Writer wr;
    for(int i=0; i<N; i++)
        wr.write_utf8(buf[i]);

    printf("read\n");
    Reader rd(wr.buffer(), wr.tell());
    for(int i=0; i<N; i++)
        assert(rd.read_utf8() == buf[i]);

    talloc_free(wr.buffer());
    free(buf);
}

static void endian_test()
{
    Writer wr;
    wr.write_u8(0x01);
    wr.write_u16(0x0203);
    wr.write_u24(0x040506);
    wr.write_u32(0x0708090a);

    Reader rd(wr.buffer(), wr.tell());

    for(int i=0; i<10; i++)
        printf("%02X ", rd.read_u8());
    printf("\n");

    rd.seek(0, SEEK_SET);
    assert(rd.read_u8() == 0x01);
    assert(rd.read_u16() == 0x0203);
    assert(rd.read_u24() == 0x040506);
    assert(rd.read_u32() == 0x0708090a);

    talloc_free(wr.buffer());
}

int main(void)
{
    utf8_test();
    uv_test();
    endian_test();
}
