/* lapack.c - replacement for Intel's libLAPACK (missing in the Combo Update; vecLib loads it). Only dgels_ and
 * sgesvd_ are imported (VideoConference, QuartzComposer); both report an error (info = -1) instead of computing. */
#include <stdio.h>

typedef long integer;                          /* __CLPK_integer on i386 */

static void report(const char *name)
{
    static int reported;
    if (!reported++) fprintf(stderr, "rosetta replacement: LAPACK %s missing (returns an error)\n", name);
}

int dgels_(char *trans, integer *m, integer *n, integer *nrhs, double *a, integer *lda, double *b, integer *ldb, double *work,
           integer *lwork, integer *info)
{
    (void)trans; (void)m; (void)n; (void)nrhs; (void)a; (void)lda; (void)b; (void)ldb; (void)work; (void)lwork;
    report("dgels_");
    *info = -1;
    return 0;
}

int sgesvd_(char *jobu, char *jobvt, integer *m, integer *n, float *a, integer *lda, float *s, float *u, integer *ldu, float *vt,
            integer *ldvt, float *work, integer *lwork, integer *info)
{
    (void)jobu; (void)jobvt; (void)m; (void)n; (void)a; (void)lda; (void)s; (void)u; (void)ldu; (void)vt; (void)ldvt;
    (void)work; (void)lwork;
    report("sgesvd_");
    *info = -1;
    return 0;
}
