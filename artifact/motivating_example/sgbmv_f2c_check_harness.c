#include<stdlib.h>
#include<stdio.h>

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/* Subroutine */ int sgbmv(char *trans, int *m, int *n, int *kl, 
	int *ku, float *alpha, float *a, int *lda, float *x, int *
	incx, float *beta, float *y, int *incy)
{
    /* System generated locals */
    int a_dim1, a_offset, i__1, i__2, i__3, i__4, i__5, i__6;

    /* Local variables */
    int i__, j, k, ix, iy, jx, jy, kx, ky, kup1, info;
    float temp;
    int lenx, leny;

/*     .. Scalar Arguments .. */
/*     .. */
/*     .. Array Arguments .. */
/*     .. */

/*  Purpose */
/*  ======= */

/*  SGBMV  performs one of the matrix-vector operations */

/*     y := alpha*A*x + beta*y,   or   y := alpha*A'*x + beta*y, */

/*  where alpha and beta are scalars, x and y are vectors and A is an */
/*  m by n band matrix, with kl sub-diagonals and ku super-diagonals. */

/*  Arguments */
/*  ========== */

/*  TRANS  - CHARACTER*1. */
/*           On entry, TRANS specifies the operation to be performed as */
/*           follows: */

/*              TRANS = 'N' or 'n'   y := alpha*A*x + beta*y. */

/*              TRANS = 'T' or 't'   y := alpha*A'*x + beta*y. */

/*              TRANS = 'C' or 'c'   y := alpha*A'*x + beta*y. */

/*           Unchanged on exit. */

/*  M      - int. */
/*           On entry, M specifies the number of rows of the matrix A. */
/*           M must be at least zero. */
/*           Unchanged on exit. */

/*  N      - int. */
/*           On entry, N specifies the number of columns of the matrix A. */
/*           N must be at least zero. */
/*           Unchanged on exit. */

/*  KL     - int. */
/*           On entry, KL specifies the number of sub-diagonals of the */
/*           matrix A. KL must satisfy  0 .le. KL. */
/*           Unchanged on exit. */

/*  KU     - int. */
/*           On entry, KU specifies the number of super-diagonals of the */
/*           matrix A. KU must satisfy  0 .le. KU. */
/*           Unchanged on exit. */

/*  ALPHA  - float            . */
/*           On entry, ALPHA specifies the scalar alpha. */
/*           Unchanged on exit. */

/*  A      - float             array of DIMENSION ( LDA, n ). */
/*           Before entry, the leading ( kl + ku + 1 ) by n part of the */
/*           array A must contain the matrix of coefficients, supplied */
/*           column by column, with the leading diagonal of the matrix in */
/*           row ( ku + 1 ) of the array, the first super-diagonal */
/*           starting at position 2 in row ku, the first sub-diagonal */
/*           starting at position 1 in row ( ku + 2 ), and so on. */
/*           Elements in the array A that do not correspond to elements */
/*           in the band matrix (such as the top left ku by ku triangle) */
/*           are not referenced. */
/*           The following program segment will transfer a band matrix */
/*           from conventional full matrix storage to band storage: */

/*                 DO 20, J = 1, N */
/*                    K = KU + 1 - J */
/*                    DO 10, I = MAX( 1, J - KU ), MIN( M, J + KL ) */
/*                       A( K + I, J ) = matrix( I, J ) */
/*              10    CONTINUE */
/*              20 CONTINUE */

/*           Unchanged on exit. */

/*  LDA    - int. */
/*           On entry, LDA specifies the first dimension of A as declared */
/*           in the calling (sub) program. LDA must be at least */
/*           ( kl + ku + 1 ). */
/*           Unchanged on exit. */

/*  X      - float             array of DIMENSION at least */
/*           ( 1 + ( n - 1 )*abs( INCX ) ) when TRANS = 'N' or 'n' */
/*           and at least */
/*           ( 1 + ( m - 1 )*abs( INCX ) ) otherwise. */
/*           Before entry, the incremented array X must contain the */
/*           vector x. */
/*           Unchanged on exit. */

/*  INCX   - int. */
/*           On entry, INCX specifies the increment for the elements of */
/*           X. INCX must not be zero. */
/*           Unchanged on exit. */

/*  BETA   - float            . */
/*           On entry, BETA specifies the scalar beta. When BETA is */
/*           supplied as zero then Y need not be set on input. */
/*           Unchanged on exit. */

/*  Y      - float             array of DIMENSION at least */
/*           ( 1 + ( m - 1 )*abs( INCY ) ) when TRANS = 'N' or 'n' */
/*           and at least */
/*           ( 1 + ( n - 1 )*abs( INCY ) ) otherwise. */
/*           Before entry, the incremented array Y must contain the */
/*           vector y. On exit, Y is overwritten by the updated vector y. */

/*  INCY   - int. */
/*           On entry, INCY specifies the increment for the elements of */
/*           Y. INCY must not be zero. */
/*           Unchanged on exit. */


/*  Level 2 Blas routine. */

/*  -- Written on 22-October-1986. */
/*     Jack Dongarra, Argonne National Lab. */
/*     Jeremy Du Croz, Nag Central Office. */
/*     Sven Hammarling, Nag Central Office. */
/*     Richard Hanson, Sandia National Labs. */

/*     .. Parameters .. */
/*     .. */
/*     .. Local Scalars .. */
/*     .. */
/*     .. External Functions .. */
/*     .. */
/*     .. External Subroutines .. */
/*     .. */
/*     .. Intrinsic Functions .. */
/*     .. */

/*     Test the input parameters. */

    /* Parameter adjustments */
    a_dim1 = *lda;
    a_offset = 1 + a_dim1;
    a -= a_offset;
    --x;
    --y;

    /* Function Body */
    info = 0;
    if ( *trans != 'N' && *trans != 'T' && *trans != 'C'){
	info = 1;
    } else if (*m < 0) {
	info = 2;
    } else if (*n < 0) {
	info = 3;
    } else if (*kl < 0) {
	info = 4;
    } else if (*ku < 0) {
	info = 5;
    } else if (*lda < *kl + *ku + 1) {
	info = 8;
    } else if (*incx == 0) {
	info = 10;
    } else if (*incy == 0) {
	info = 13;
    }
    if (info != 0) {
	return 0;
    }

/*     Quick return if possible. */

    if (*m == 0 || *n == 0 || *alpha == 0.f && *beta == 1.f) {
	return 0;
    }

/*     Set  LENX  and  LENY, the lengths of the vectors x and y, and set */
/*     up the start points in  X  and  Y. */

    if (*trans == 'N') {
	lenx = *n;
	leny = *m;
    } else {
	lenx = *m;
	leny = *n;
    }
    if (*incx > 0) {
	kx = 1;
    } else {
	kx = 1 - (lenx - 1) * *incx;
    }
    if (*incy > 0) {
	ky = 1;
    } else {
	ky = 1 - (leny - 1) * *incy;
    }

/*     Start the operations. In this version the elements of A are */
/*     accessed sequentially with one pass through the band part of A. */

/*     First form  y := beta*y. */

    if (*beta != 1.f) {
	if (*incy == 1) {
	    if (*beta == 0.f) {
		i__1 = leny;
		for (i__ = 1; i__ <= i__1; ++i__) {
		    y[i__] = 0.f;
/* L10: */
		}
	    } else {
		i__1 = leny;
		for (i__ = 1; i__ <= i__1; ++i__) {
            if ( *beta * y[i__] != *beta * y[i__] ){
                // printf("\t(1) y[%d] <- beta * y[%d]\n", i__, i__);
                printf("!");
            }
		    y[i__] = *beta * y[i__];
/* L20: */
		}
	    }
	} else {
	    iy = ky;
	    if (*beta == 0.f) {
		i__1 = leny;
		for (i__ = 1; i__ <= i__1; ++i__) {
		    y[iy] = 0.f;
		    iy += *incy;
/* L30: */
		}
	    } else {
		i__1 = leny;
		for (i__ = 1; i__ <= i__1; ++i__) {
            if ( *beta * y[iy] != *beta * y[iy] ){
                // printf("\t(2) y[%d] <- beta * y[%d]\n", iy, iy);
                printf("!");
            }
		    y[iy] = *beta * y[iy];
		    iy += *incy;
/* L40: */
		}
	    }
	}
    }
    if (*alpha == 0.f) {
	return 0;
    }
    kup1 = *ku + 1;
    if ( *trans == 'N') {

/*        Form  y := alpha*A*x + y. */

	jx = kx;
	if (*incy == 1) {
	    i__1 = *n;
	    for (j = 1; j <= i__1; ++j) {
		if (x[jx] != 0.f) {
            if ( *alpha * x[jx] != *alpha * x[jx] ){
                // printf("\t(3) temp <- alpha * x[%d]\n", jx);
                printf("!");
            }
		    temp = *alpha * x[jx];
		    k = kup1 - j;
/* Computing MAX */
		    i__2 = 1, i__3 = j - *ku;
/* Computing MIN */
		    i__5 = *m, i__6 = j + *kl;
		    i__4 = MIN(i__5,i__6);
		    for (i__ = MAX(i__2,i__3); i__ <= i__4; ++i__) {
                if ( temp * a[k + i__ + j * a_dim1] != temp * a[k + i__ + j * a_dim1] ){
                    // printf("\t(4.1) temp * a[%d]\n", k + i__ + j * a_dim1);
                    printf("!");
                }
                if ( y[i__] + temp * a[k + i__ + j * a_dim1] != y[i__] + temp * a[k + i__ + j * a_dim1] ){
                    // printf("\t(4.2) y[%d] <- y[%d] + temp * a[%d]\n", i__, i__, k + i__ + j * a_dim1);
                    printf("!");
                }
			y[i__] += temp * a[k + i__ + j * a_dim1];
/* L50: */
		    }
		}
		jx += *incx;
/* L60: */
	    }
	} else {
	    i__1 = *n;
	    for (j = 1; j <= i__1; ++j) {
		if (x[jx] != 0.f) {
            if ( *alpha * x[jx] != *alpha * x[jx] ){
                // printf("\t(5) temp = alpha * x[%d]\n", jx);
                printf("!");
            }
		    temp = *alpha * x[jx];
		    iy = ky;
		    k = kup1 - j;
/* Computing MAX */
		    i__4 = 1, i__2 = j - *ku;
/* Computing MIN */
		    i__5 = *m, i__6 = j + *kl;
		    i__3 = MIN(i__5,i__6);
		    for (i__ = MAX(i__4,i__2); i__ <= i__3; ++i__) {
                if ( temp * a[k + i__ + j * a_dim1] != temp * a[k + i__ + j * a_dim1] ){
                    // printf("\t(6.1) temp * a[%d]\n", k + i__ + j * a_dim1);
                    printf("!");
                }
                if ( y[iy] + temp * a[k + i__ + j * a_dim1] != y[iy] + temp * a[k + i__ + j * a_dim1] ){
                    // printf("\t(6.2) y[%d] <- y[%d] + temp * a[%d]\n", iy, iy, k + i__ + j * a_dim1);
                    printf("!");
                }
			y[iy] += temp * a[k + i__ + j * a_dim1];
			iy += *incy;
/* L70: */
		    }
		}
		jx += *incx;
		if (j > *ku) {
		    ky += *incy;
		}
/* L80: */
	    }
	}
    } else {

/*        Form  y := alpha*A'*x + y. */

	jy = ky;
	if (*incx == 1) {
	    i__1 = *n;
	    for (j = 1; j <= i__1; ++j) {
		temp = 0.f;
		k = kup1 - j;
/* Computing MAX */
		i__3 = 1, i__4 = j - *ku;
/* Computing MIN */
		i__5 = *m, i__6 = j + *kl;
		i__2 = MIN(i__5,i__6);
		for (i__ = MAX(i__3,i__4); i__ <= i__2; ++i__) {
            if ( a[k + i__ + j * a_dim1] * x[i__] != a[k + i__ + j * a_dim1] * x[i__] ){
                // printf("\t(9) a[%d] * x[%d]\n", k + i__ + j * a_dim1, i__);
                printf("!");
            }
            if ( temp + a[k + i__ + j * a_dim1] * x[i__] != temp + a[k + i__ + j * a_dim1] * x[i__] ){
                // printf("\t(10) temp + a[%d] * x[%d]\n", k + i__ + j * a_dim1, i__);
                printf("!");
            }
		    temp += a[k + i__ + j * a_dim1] * x[i__];
/* L90: */
		}
        if ( *alpha * temp != *alpha * temp ){
            // printf("\t(11) alpha * temp\n");
            printf("!");
        }
        if ( y[jy] + *alpha * temp != y[jy] + *alpha * temp ){
            // printf("\t(12) y[%d] + alpha * temp\n", jy);
            printf("!");
        }
        y[jy] += *alpha * temp;
		jy += *incy;
/* L100: */
	    }
	} else {
	    i__1 = *n;
	    for (j = 1; j <= i__1; ++j) {
		temp = 0.f;
		ix = kx;
		k = kup1 - j;
/* Computing MAX */
		i__2 = 1, i__3 = j - *ku;
/* Computing MIN */
		i__5 = *m, i__6 = j + *kl;
		i__4 = MIN(i__5,i__6);
		for (i__ = MAX(i__2,i__3); i__ <= i__4; ++i__) {
            if ( a[k + i__ + j * a_dim1] * x[ix] != a[k + i__ + j * a_dim1] * x[ix] ){
                // printf("\t(13) a[%d] * x[%d]\n", k + i__ + j * a_dim1, ix);
                printf("!");
            }
            if ( temp + a[k + i__ + j * a_dim1] * x[ix] != temp + a[k + i__ + j * a_dim1] * x[ix] ){
                // printf("\t(14) temp + a[%d] * x[%d]\n", k + i__ + j * a_dim1, ix);
                printf("!");
            }
		    temp += a[k + i__ + j * a_dim1] * x[ix];
		    ix += *incx;
/* L110: */
		}
        if ( *alpha * temp != *alpha * temp ){
            // printf("\t(15) alpha * temp\n");
            printf("!");
        }
        if ( y[jy] + *alpha * temp != y[jy] + *alpha * temp ){
            // printf("\t(16) y[%d] + alpha * temp\n", jy);
            printf("!");
        }
		y[jy] += *alpha * temp;
		jy += *incy;
		if (j > *ku) {
		    kx += *incx;
		}
/* L120: */
	    }
	}
    }

    return 0;

/*     End of SGBMV . */

} /* sgbmv_ */


int main(void){

    char trans =;
    int m =;
    int n =;
    int kl =;
    int ku =;
    float alpha;
    float* a;
    int lda =;
    float* x1;
    int incx =;
    float beta;
    float* y;
    int incy =;
    int i,j;

    if ( trans == 'N' ){

        float x[1+2 + lda*n + (1 + ( n - 1 )*abs( incx )) + (1 + ( m - 1 )*abs( incy ))];

#include<__>
            
        alpha = x[1+0];
        beta = x[1+1];
    
        a = malloc(lda*n*sizeof(float));
        for ( i = 0; i < lda*n; ++i ){
            a[i] = x[1+1+i];
        }
    
        x1 = malloc( (1 + ( n - 1 )*abs( incx )) * sizeof(float));
        y = malloc( (1 + ( m - 1 )*abs( incy )) * sizeof(float));

        for ( i = 0; i < 1 + ( n - 1 )*abs( incx ); ++i ){
            x1[i] = x[1+2+lda*n+i];
        }
        for ( i = 0; i < 1 + ( m - 1 )*abs( incy ); ++i ){
            y[i] = x[1+2+lda*n+1 + ( n - 1 )*abs( incx )+i];
        }
    }
    else{

        float x[1+2 + lda*n + (1 + ( m - 1 )*abs( incx )) + (1 + ( n - 1 )*abs( incy ))];

#include<__>
            
        alpha = x[1+0];
        beta = x[1+1];
    
        a = malloc(lda*n*sizeof(float));
        for ( i = 0; i < lda*n; ++i ){
            a[i] = x[1+1+i];
        }

        x1 = malloc( (1 + ( m - 1 )*abs( incx )) * sizeof(float));
        y = malloc( (1 + ( n - 1 )*abs( incy )) * sizeof(float));

        for ( i = 0; i < 1 + ( m - 1 )*abs( incx ); ++i ){
            x1[i] = x[1+2+lda*n+i];
        }
        for ( i = 0; i < 1 + ( n - 1 )*abs( incy ); ++i ){
            y[i] = x[1+2+lda*n+1 + ( m - 1 )*abs( incx )+i];
        }
    }

    sgbmv(&trans, &m, &n, &kl, &ku, &alpha, a, &lda, x1, &incx, &beta, y, &incy);

    int maxi;
    if ( trans == 'N' ){
        maxi = 1 + ( m - 1 )*abs( incy );
    }
    else{
        maxi = 1 + ( n - 1 )*abs( incy );
    }

    printf(" (out) y : ");
    for ( i = 0; i < maxi; ++i ){
        printf("%lf ", y[i]);
    }
    printf("\n");

    free(a);
    free(x1);
    free(y);

    return 0;
}