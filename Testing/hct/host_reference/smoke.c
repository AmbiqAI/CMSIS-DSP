/* Calls a few families of the host reference, compiled and linked only from what its manifest says.
   Exits 0 when every result is the exact expected value. */
#include <stdio.h>
#include <string.h>

#include "arm_math.h"
#include "arm_math_f16.h"

static int failures;

static void expect(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void basic_math(void)
{
    const float32_t a[5] = {1.0f, -2.0f, 3.5f, 0.0f, 1e30f};
    const float32_t b[5] = {0.5f, 2.0f, -1.5f, -0.0f, 1e30f};
    const float32_t sum[5] = {1.5f, 0.0f, 2.0f, 0.0f, 2e30f};
    float32_t out[5];
    arm_add_f32(a, b, out, 5);
    expect(memcmp(out, sum, sizeof out) == 0, "arm_add_f32");

    const q15_t qa[3] = {32000, -32000, 100};
    const q15_t qb[3] = {1000, -1000, -50};
    const q15_t qsum[3] = {32767, -32768, 50};
    q15_t qout[3];
    arm_add_q15(qa, qb, qout, 3);
    expect(memcmp(qout, qsum, sizeof qout) == 0, "arm_add_q15 saturates");

    const float16_t ha[2] = {(float16_t)1.5f, (float16_t)-0.25f};
    const float16_t hb[2] = {(float16_t)0.5f, (float16_t)0.25f};
    float16_t hout[2];
    arm_add_f16(ha, hb, hout, 2);
    expect((float)hout[0] == 2.0f && (float)hout[1] == 0.0f, "arm_add_f16");
}

static void matrix(void)
{
    float32_t a_data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float32_t b_data[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    float32_t c_data[4];
    const float32_t product[4] = {19.0f, 22.0f, 43.0f, 50.0f};
    arm_matrix_instance_f32 a, b, c;
    arm_mat_init_f32(&a, 2, 2, a_data);
    arm_mat_init_f32(&b, 2, 2, b_data);
    arm_mat_init_f32(&c, 2, 2, c_data);
    expect(arm_mat_mult_f32(&a, &b, &c) == ARM_MATH_SUCCESS, "arm_mat_mult_f32 status");
    expect(memcmp(c_data, product, sizeof c_data) == 0, "arm_mat_mult_f32");
}

static void filtering(void)
{
    const float32_t coeffs[3] = {0.25f, 0.5f, 0.25f};
    float32_t state[3 + 4 - 1];
    const float32_t in[4] = {4.0f, 0.0f, 0.0f, 8.0f};
    const float32_t expected[4] = {1.0f, 2.0f, 1.0f, 2.0f};
    float32_t out[4];
    arm_fir_instance_f32 fir;
    arm_fir_init_f32(&fir, 3, coeffs, state, 4);
    arm_fir_f32(&fir, in, out, 4);
    expect(memcmp(out, expected, sizeof out) == 0, "arm_fir_f32");
}

static void transform(void)
{
    float32_t in[32] = {0};
    float32_t out[32];
    arm_rfft_fast_instance_f32 rfft;
    in[0] = 1.0f;
    if (arm_rfft_fast_init_f32(&rfft, 32) != ARM_MATH_SUCCESS) {
        expect(0, "arm_rfft_fast_init_f32 status");
        return;
    }
    arm_rfft_fast_f32(&rfft, in, out, 0);
    /* An impulse has a flat spectrum: every bin is 1 + 0i, DC and Nyquist packed into out[0], out[1]. */
    int flat = 1;
    for (int k = 0; k < 32; k++) {
        flat &= out[k] == ((k % 2 == 0 || k == 1) ? 1.0f : 0.0f);
    }
    expect(flat, "arm_rfft_fast_f32 impulse");
}

int main(void)
{
    basic_math();
    matrix();
    filtering();
    transform();
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
