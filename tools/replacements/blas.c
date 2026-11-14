/* blas.c - replacement for Intel's libBLAS (missing in the Combo Update; vecLib loads it): the six float routines
 * that i386 images of the Intel root import, as a reference implementation in C (i386, translated by rosetta). */
enum ORDER { LINES = 101, COLS = 102 };
enum SWAP { NOT = 111, SWAPPED = 112, CONJ = 113 };

int cblas_isamax(const int N, const float *X, const int incX)
{
    int i, best = 0;
    float m = -1;
    if (N <= 0 || incX <= 0) return 0;
    for (i = 0; i < N; i++) {
        float v = X[i * incX] < 0 ? -X[i * incX] : X[i * incX];
        if (v > m) { m = v; best = i; }
    }
    return best;
}

void cblas_saxpy(const int N, const float alpha, const float *X, const int incX, float *Y, const int incY)
{
    int i, ix = incX < 0 ? (1 - N) * incX : 0, iy = incY < 0 ? (1 - N) * incY : 0;
    for (i = 0; i < N; i++, ix += incX, iy += incY) Y[iy] += alpha * X[ix];
}

void cblas_scopy(const int N, const float *X, const int incX, float *Y, const int incY)
{
    int i, ix = incX < 0 ? (1 - N) * incX : 0, iy = incY < 0 ? (1 - N) * incY : 0;
    for (i = 0; i < N; i++, ix += incX, iy += incY) Y[iy] = X[ix];
}

float cblas_sdot(const int N, const float *X, const int incX, const float *Y, const int incY)
{
    int i, ix = incX < 0 ? (1 - N) * incX : 0, iy = incY < 0 ? (1 - N) * incY : 0;
    float s = 0;
    for (i = 0; i < N; i++, ix += incX, iy += incY) s += X[ix] * Y[iy];
    return s;
}

void cblas_sscal(const int N, const float alpha, float *X, const int incX)
{
    int i;
    if (incX <= 0) return;
    for (i = 0; i < N; i++) X[i * incX] *= alpha;
}

/* y = alpha * op(A) * x + beta * y */
void cblas_sgemv(const enum ORDER Order, const enum SWAP TransA, const int M, const int N, const float alpha,
                 const float *A, const int lda, const float *X, const int incX, const float beta, float *Y, const int incY)
{
    int swapped = (TransA != NOT) ^ (Order == COLS);   /* row-major form: A[i * lda + j] */
    int lenY = TransA == NOT ? M : N, lenX = TransA == NOT ? N : M, i, j;
    int ix0 = incX < 0 ? (1 - lenX) * incX : 0, iy = incY < 0 ? (1 - lenY) * incY : 0;
    for (i = 0; i < lenY; i++, iy += incY) {
        float s = 0;
        int ix = ix0;
        for (j = 0; j < lenX; j++, ix += incX) s += (swapped ? A[j * lda + i] : A[i * lda + j]) * X[ix];
        Y[iy] = alpha * s + (beta == 0 ? 0 : beta * Y[iy]);
    }
}
