#ifndef __M__
#define __M__
/*
*	DirectX-compliant, ie row-column order, ie m[Row][Col].
*	Same as:
*	m11  m12  m13  m14	first row.
*	m21  m22  m23  m24	second row.
*	m31  m32  m33  m34	third row.
*	m41  m42  m43  m44	fourth row.
*	Translation is (m41, m42, m43), (m14, m24, m34, m44) = (0, 0, 0, 1).
*	Stored in memory as m11 m12 m13 m14 m21...
*
*	Multiplication rules:
*
*	[x'y'z'1] = [xyz1][M]
*
*	x' = x*m11 + y*m21 + z*m31 + m41
*	y' = x*m12 + y*m22 + z*m32 + m42
*	z' = x*m13 + y*m23 + z*m33 + m43
*	1' =     0 +     0 +     0 + m44
*/

// NOTE_1: positive angle means clockwise rotation
// NOTE_2: mul(A,B) means transformation B, followed by A
// NOTE_3: I,J,K,C equals to R,N,D,T
// NOTE_4: The rotation sequence is ZXY
#include "xmmintrin.h"
#define MakeShuffleMask(x,y,z,w)           (x | (y<<2) | (z<<4) | (w<<6))

// vec(0, 1, 2, 3) -> (vec[x], vec[y], vec[z], vec[w])
#define VecSwizzleMask(vec, mask)          _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(vec), mask))
#define VecSwizzle(vec, x, y, z, w)        VecSwizzleMask(vec, MakeShuffleMask(x,y,z,w))
#define VecSwizzle1(vec, x)                VecSwizzleMask(vec, MakeShuffleMask(x,x,x,x))

// return (vec1[x], vec1[y], vec2[z], vec2[w])
#define VecShuffle(vec1, vec2, x,y,z,w)    _mm_shuffle_ps(vec1, vec2, MakeShuffleMask(x,y,z,w))
// special shuffle
#define VecShuffle_0101(vec1, vec2)        _mm_movelh_ps(vec1, vec2)
#define VecShuffle_2323(vec1, vec2)        _mm_movehl_ps(vec2, vec1)

#define SHUFFLE_0000 0x00
#define SHUFFLE_1111 0x55
#define SHUFFLE_2222 0xaa
#define SHUFFLE_3333 0xff

template <class T>
struct _MM_ALIGN16 _matrix
{
	typedef _matrix<T>	Self;
	typedef Self&		SelfRef;
	typedef const Self&	SelfCRef;
	typedef _vector3<T>	Tvector;

	union {
		struct {						// Direct definition
            T _11, _12, _13, _14;
            T _21, _22, _23, _24;
            T _31, _32, _33, _34;
            T _41, _42, _43, _44;
		};
    	struct{
    		Tvector i;	T	_14_;
    		Tvector j;	T	_24_;
    		Tvector k;	T	_34_;
    		Tvector c;	T	_44_;
        };
		T m[4][4];					// Array
		T mm[16];
		__m128 xmm[4];
		_vector4<T> row[4];
	};

	ICF	SelfRef	set			(const Self &a) 
	{
		i.set(a.i); _14_=a._14;
		j.set(a.j); _24_=a._24;
		k.set(a.k); _34_=a._34;
		c.set(a.c); _44_=a._44;
		return *this;
	}
	ICF	SelfRef	set			(const Tvector& R,const Tvector& N,const Tvector& D,const Tvector& C) 
	{
		i.set(R); _14_=0;
		j.set(N); _24_=0;
		k.set(D); _34_=0;
		c.set(C); _44_=1;
		return *this;
	}
	ICF	SelfRef	identity	(void) 
	{
		_11=1; _12=0; _13=0; _14=0;
		_21=0; _22=1; _23=0; _24=0;
		_31=0; _32=0; _33=1; _34=0;
		_41=0; _42=0; _43=0; _44=1;
		return *this;
	}
	IC	SelfRef	rotation	(const _quaternion<T> &Q);
	ICF	SelfRef	mk_xform	(const _quaternion<T> &Q, const Tvector &V);
	// Multiply RES = A[4x4]*B[4x4] (WITH projection)
	ICF	SelfRef	mul(const Self& A, const Self& B)
	{
		VERIFY((this != &A) && (this != &B));

		xmm[0] = _mm_add_ps(_mm_add_ps(_mm_add_ps(
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[0], B.xmm[0], SHUFFLE_0000), A.xmm[0]),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[0], B.xmm[0], SHUFFLE_1111), A.xmm[1])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[0], B.xmm[0], SHUFFLE_2222), A.xmm[2])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[0], B.xmm[0], SHUFFLE_3333), A.xmm[3]));

		xmm[1] = _mm_add_ps(_mm_add_ps(_mm_add_ps(
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[1], B.xmm[1], SHUFFLE_0000), A.xmm[0]),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[1], B.xmm[1], SHUFFLE_1111), A.xmm[1])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[1], B.xmm[1], SHUFFLE_2222), A.xmm[2])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[1], B.xmm[1], SHUFFLE_3333), A.xmm[3]));

		xmm[2] = _mm_add_ps(_mm_add_ps(_mm_add_ps(
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[2], B.xmm[2], SHUFFLE_0000), A.xmm[0]),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[2], B.xmm[2], SHUFFLE_1111), A.xmm[1])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[2], B.xmm[2], SHUFFLE_2222), A.xmm[2])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[2], B.xmm[2], SHUFFLE_3333), A.xmm[3]));

		xmm[3] = _mm_add_ps(_mm_add_ps(_mm_add_ps(
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[3], B.xmm[3], SHUFFLE_0000), A.xmm[0]),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[3], B.xmm[3], SHUFFLE_1111), A.xmm[1])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[3], B.xmm[3], SHUFFLE_2222), A.xmm[2])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[3], B.xmm[3], SHUFFLE_3333), A.xmm[3]));

		return *this;
	};
	// Multiply RES = A[4x3]*B[4x3] (no projection)
	ICF	SelfRef	mul_43(const Self& A, const Self& B)
	{
		VERIFY((this != &A) && (this != &B));

		xmm[0] = _mm_add_ps(_mm_add_ps(
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[0], B.xmm[0], SHUFFLE_0000), A.xmm[0]),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[0], B.xmm[0], SHUFFLE_1111), A.xmm[1])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[0], B.xmm[0], SHUFFLE_2222), A.xmm[2]));

		xmm[1] = _mm_add_ps(_mm_add_ps(
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[1], B.xmm[1], SHUFFLE_0000), A.xmm[0]),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[1], B.xmm[1], SHUFFLE_1111), A.xmm[1])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[1], B.xmm[1], SHUFFLE_2222), A.xmm[2]));

		xmm[2] = _mm_add_ps(_mm_add_ps(
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[2], B.xmm[2], SHUFFLE_0000), A.xmm[0]),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[2], B.xmm[2], SHUFFLE_1111), A.xmm[1])),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[2], B.xmm[2], SHUFFLE_2222), A.xmm[2]));

		xmm[3] = _mm_add_ps(_mm_add_ps(
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[3], B.xmm[3], SHUFFLE_0000), A.xmm[0]),
				 _mm_mul_ps(_mm_shuffle_ps(B.xmm[3], B.xmm[3], SHUFFLE_1111), A.xmm[1])),
				 _mm_add_ps(_mm_mul_ps(_mm_shuffle_ps(B.xmm[3], B.xmm[3], SHUFFLE_2222), A.xmm[2]), A.xmm[3]));

		m[0][3] = 0;
		m[1][3] = 0;
		m[2][3] = 0;
		m[3][3] = T(1);

		return *this;
	};
	IC	SelfRef	mulA_44		( const Self &A )			// mul after 
	{
    	Self B; B.set( *this ); 	mul		( A, B );
		return *this;
    };
	IC	SelfRef	mulB_44		( const Self &B )			// mul before
	{
		Self A; A.set( *this ); 	mul		( A, B );
		return *this;
	};
	ICF	SelfRef	mulA_43		( const Self &A )			// mul after (no projection)
	{
    	Self B; B.set( *this ); 	mul_43	( A, B );
		return *this;
    };
	ICF	SelfRef	mulB_43		( const Self &B )			// mul before (no projection)
	{
		Self A; A.set( *this ); 	mul_43	( A, B );
		return *this;
	};
	ICF __m128 Mat2Mul(__m128 vec1, __m128 vec2)
	{
		return
			_mm_add_ps(_mm_mul_ps(                     vec1, VecSwizzle(vec2, 0,0,3,3)),
			           _mm_mul_ps(VecSwizzle(vec1, 2,3,0,1), VecSwizzle(vec2, 1,1,2,2)));
	}
	// 2x2 column major Matrix adjugate multiply (A#)*B
	ICF __m128 Mat2AdjMul(__m128 vec1, __m128 vec2)
	{
		return
			_mm_sub_ps(_mm_mul_ps(VecSwizzle(vec1, 3,0,3,0), vec2),
			           _mm_mul_ps(VecSwizzle(vec1, 2,1,2,1), VecSwizzle(vec2, 1,0,3,2)));
	
	}
	// 2x2 column major Matrix multiply adjugate A*(B#)
	ICF __m128 Mat2MulAdj(__m128 vec1, __m128 vec2)
	{
		return
			_mm_sub_ps(_mm_mul_ps(                     vec1, VecSwizzle(vec2, 3,3,0,0)),
			           _mm_mul_ps(VecSwizzle(vec1, 2,3,0,1), VecSwizzle(vec2, 1,1,2,2)));
	}

	// important: this is 4x4 invert
	IC	SelfRef	invert44( const Self &a )
	{
			// use block matrix method
			// A is a matrix, then i(A) or iA means inverse of A, A# (or A_ in code) means adjugate of A, |A| (or detA in code) is determinant, tr(A) is trace
		
			// sub matrices
			__m128 A = VecShuffle_0101(a.xmm[0], a.xmm[1]);
			__m128 C = VecShuffle_2323(a.xmm[0], a.xmm[1]);
			__m128 B = VecShuffle_0101(a.xmm[2], a.xmm[3]);
			__m128 D = VecShuffle_2323(a.xmm[2], a.xmm[3]);
		
			__m128 detSub = _mm_sub_ps(
				_mm_mul_ps(VecShuffle(a.xmm[0], a.xmm[2], 0,2,0,2), VecShuffle(a.xmm[1], a.xmm[3], 1,3,1,3)),
				_mm_mul_ps(VecShuffle(a.xmm[0], a.xmm[2], 1,3,1,3), VecShuffle(a.xmm[1], a.xmm[3], 0,2,0,2))
				);
			__m128 detA = VecSwizzle1(detSub, 0);
			__m128 detC = VecSwizzle1(detSub, 1);
			__m128 detB = VecSwizzle1(detSub, 2);
			__m128 detD = VecSwizzle1(detSub, 3);

			__m128 D_C = Mat2AdjMul(D, C);
			__m128 A_B = Mat2AdjMul(A, B);
			__m128 X_ = _mm_sub_ps(_mm_mul_ps(detD, A), Mat2Mul(B, D_C));
			__m128 W_ = _mm_sub_ps(_mm_mul_ps(detA, D), Mat2Mul(C, A_B));
			__m128 detM = _mm_mul_ps(detA, detD);
			__m128 Y_ = _mm_sub_ps(_mm_mul_ps(detB, C), Mat2MulAdj(D, A_B));
			__m128 Z_ = _mm_sub_ps(_mm_mul_ps(detC, B), Mat2MulAdj(A, D_C));
			detM = _mm_add_ps(detM, _mm_mul_ps(detB, detC));
			__m128 tr = _mm_mul_ps(A_B, VecSwizzle(D_C, 0,2,1,3));
			tr = _mm_hadd_ps(tr, tr);
			tr = _mm_hadd_ps(tr, tr);
			detM = _mm_sub_ps(detM, tr);
		
			const __m128 adjSignMask = _mm_setr_ps(1.f, -1.f, -1.f, 1.f);
			__m128 rDetM = _mm_div_ps(adjSignMask, detM);
		
			X_ = _mm_mul_ps(X_, rDetM);
			Y_ = _mm_mul_ps(Y_, rDetM);
			Z_ = _mm_mul_ps(Z_, rDetM);
			W_ = _mm_mul_ps(W_, rDetM);
		
			// apply adjugate and store, here we combine adjugate shuffle and store shuffle
			xmm[0] = VecShuffle(X_, Z_, 3,1,3,1);
			xmm[1] = VecShuffle(X_, Z_, 2,0,2,0);
			xmm[2] = VecShuffle(Y_, W_, 3,1,3,1);
			xmm[3] = VecShuffle(Y_, W_, 2,0,2,0);

		return *this;
	}

	// important: this is 4x3 invert, not the 4x4 one
	IC	SelfRef	invert( const Self &a )
	{
		// faster than self-invert
		T fDetInv = ( a._11 * ( a._22 * a._33 - a._23 * a._32 ) -
			a._12 * ( a._21 * a._33 - a._23 * a._31 ) +
			a._13 * ( a._21 * a._32 - a._22 * a._31 ) );

		VERIFY(_abs(fDetInv)>flt_zero);
		fDetInv=1.0f/fDetInv;

		_11 =  fDetInv * ( a._22 * a._33 - a._23 * a._32 );
		_12 = -fDetInv * ( a._12 * a._33 - a._13 * a._32 );
		_13 =  fDetInv * ( a._12 * a._23 - a._13 * a._22 );
		_14 = 0.0f;

		_21 = -fDetInv * ( a._21 * a._33 - a._23 * a._31 );
		_22 =  fDetInv * ( a._11 * a._33 - a._13 * a._31 );
		_23 = -fDetInv * ( a._11 * a._23 - a._13 * a._21 );
		_24 = 0.0f;

		_31 =  fDetInv * ( a._21 * a._32 - a._22 * a._31 );
		_32 = -fDetInv * ( a._11 * a._32 - a._12 * a._31 );
		_33 =  fDetInv * ( a._11 * a._22 - a._12 * a._21 );
		_34 = 0.0f;

		_41 = -( a._41 * _11 + a._42 * _21 + a._43 * _31 );
		_42 = -( a._41 * _12 + a._42 * _22 + a._43 * _32 );
		_43 = -( a._41 * _13 + a._42 * _23 + a._43 * _33 );
		_44 = 1.0f;
		return *this;
	}

	// important: this is 4x3 invert, not the 4x4 one
	IC	bool	invert_b	( const Self &a )
	{
		// faster than self-invert
		T fDetInv = ( a._11 * ( a._22 * a._33 - a._23 * a._32 ) -
			a._12 * ( a._21 * a._33 - a._23 * a._31 ) +
			a._13 * ( a._21 * a._32 - a._22 * a._31 ) );

		if (_abs(fDetInv)<=flt_zero)	return	false;
		fDetInv=1.0f/fDetInv;

		_11 =  fDetInv * ( a._22 * a._33 - a._23 * a._32 );
		_12 = -fDetInv * ( a._12 * a._33 - a._13 * a._32 );
		_13 =  fDetInv * ( a._12 * a._23 - a._13 * a._22 );
		_14 = 0.0f;

		_21 = -fDetInv * ( a._21 * a._33 - a._23 * a._31 );
		_22 =  fDetInv * ( a._11 * a._33 - a._13 * a._31 );
		_23 = -fDetInv * ( a._11 * a._23 - a._13 * a._21 );
		_24 = 0.0f;

		_31 =  fDetInv * ( a._21 * a._32 - a._22 * a._31 );
		_32 = -fDetInv * ( a._11 * a._32 - a._12 * a._31 );
		_33 =  fDetInv * ( a._11 * a._22 - a._12 * a._21 );
		_34 = 0.0f;

		_41 = -( a._41 * _11 + a._42 * _21 + a._43 * _31 );
		_42 = -( a._41 * _12 + a._42 * _22 + a._43 * _32 );
		_43 = -( a._41 * _13 + a._42 * _23 + a._43 * _33 );
		_44 = 1.0f;
		return true;
	}

	IC SelfRef OrthographicOffCenterLH
	(
	    float ViewLeft,
	    float ViewRight,
	    float ViewBottom,
	    float ViewTop,
	    float NearZ,
	    float FarZ
	)
	{
	    float ReciprocalWidth = 1.0f / (ViewRight - ViewLeft);
	    float ReciprocalHeight = 1.0f / (ViewTop - ViewBottom);
	    float fRange = 1.0f / (FarZ - NearZ);

	    m[0][0] = ReciprocalWidth + ReciprocalWidth;
	    m[0][1] = 0.0f;
	    m[0][2] = 0.0f;
	    m[0][3] = 0.0f;
	
	    m[1][0] = 0.0f;
	    m[1][1] = ReciprocalHeight + ReciprocalHeight;
	    m[1][2] = 0.0f;
	    m[1][3] = 0.0f;
	
	    m[2][0] = 0.0f;
	    m[2][1] = 0.0f;
	    m[2][2] = fRange;
	    m[2][3] = 0.0f;
	
	    m[3][0] = -(ViewLeft + ViewRight) * ReciprocalWidth;
	    m[3][1] = -(ViewTop + ViewBottom) * ReciprocalHeight;
	    m[3][2] = -fRange * NearZ;
	    m[3][3] = 1.0f;
	    return *this;
	}

	IC	SelfRef	invert		()					// slower than invert other matrix
	{
		Self a;	a.set(*this);	invert(a);
		return *this;
	}
	IC	SelfRef	invert44		()				// slower than invert other matrix
	{
		Self a;	a.set(*this);	invert44(a);
		return *this;
	}

	IC	SelfRef	transpose	(const Self &matSource)	// faster version of transpose
	{
		_11=matSource._11;	_12=matSource._21;	_13=matSource._31;	_14=matSource._41;
		_21=matSource._12;	_22=matSource._22;	_23=matSource._32;	_24=matSource._42;
		_31=matSource._13;	_32=matSource._23;	_33=matSource._33;	_34=matSource._43;
		_41=matSource._14;	_42=matSource._24;	_43=matSource._34;	_44=matSource._44;
		return *this;
	}
	IC	SelfRef	transpose	()						// self transpose - slower
	{
		Self a;	a.set(*this);	transpose(a);
		return *this;
	}
	IC	SelfRef	translate	(const Tvector &Loc )		// setup translation matrix
	{	
		identity();	c.set	(Loc.x,Loc.y,Loc.z);	
		return *this;
	}
	IC	SelfRef	translate	(T _x, T _y, T _z ) // setup translation matrix
	{	
		identity(); c.set	(_x,_y,_z);				
		return *this;
	}
	IC	SelfRef	translate_over(const Tvector &Loc )	// modify only translation
	{	
		c.set	(Loc.x,Loc.y,Loc.z);				
		return *this;
	}
	IC	SelfRef	translate_over(T _x, T _y, T _z) // modify only translation
	{	
		c.set	(_x,_y,_z);							
		return *this;
	}
	IC	SelfRef	translate_add(const Tvector &Loc )	// combine translation
	{	
		c.add	(Loc);								
		return *this;
	}
	IC	SelfRef	scale		(T x, T y, T z )	// setup scale matrix
	{	
		identity(); m[0][0]=x; m[1][1]=y; m[2][2]=z; 
		return *this;
	}
	IC	SelfRef	scale(const Tvector &v )			// setup scale matrix
	{	return scale(v.x,v.y,v.z); }

	IC	SelfRef	rotateX		(T Angle )				// rotation about X axis
	{
		T cosa	= _cos(Angle);
		T sina	= _sin(Angle);
		i.set		(1,		0,		0	);	_14 = 0;
		j.set		(0,		cosa,	sina);	_24 = 0;
		k.set		(0,    -sina,   cosa);	_34 = 0;
		c.set		(0,		0,		0	);	_44 = 1;
		return *this;
	}
	IC	SelfRef	rotateY		(T Angle )				// rotation about Y axis
	{
		T cosa	= _cos(Angle);
		T sina	= _sin(Angle);
		i.set		(cosa,	0,	   -sina);	_14 = 0;
		j.set		(0,		1,		0	);	_24 = 0;
		k.set		(sina,  0,		cosa);	_34 = 0;
		c.set		(0,		0,		0	);	_44 = 1;
		return *this;
	}
	IC	SelfRef	rotateZ		(T Angle )				// rotation about Z axis
	{
		T cosa	= _cos(Angle);
		T sina	= _sin(Angle);
		i.set		(cosa,	sina,	0	);	_14 = 0;
		j.set		(-sina,	cosa,	0	);	_24 = 0;
		k.set		(0,		0,		1	);	_34 = 0;
		c.set		(0,		0,		0	);	_44 = 1;
		return *this;
	}

	IC	SelfRef	rotation	( const Tvector &vdir, const Tvector &vnorm )	{
		Tvector vright;
		vright.crossproduct	(vnorm,vdir).normalize();
		m[0][0] = vright.x;	m[0][1] = vright.y;	m[0][2] = vright.z; m[0][3]=0;
		m[1][0] = vnorm.x;	m[1][1] = vnorm.y;	m[1][2] = vnorm.z;	m[1][3]=0;
		m[2][0] = vdir.x;	m[2][1] = vdir.y;	m[2][2] = vdir.z;	m[2][3]=0;
		m[3][0] = 0;		m[3][1] = 0;		m[3][2] = 0;		m[3][3]=1;
		return *this;
	}

	IC	SelfRef	mapXYZ		()	{i.set(1, 0, 0);_14=0;j.set(0, 1, 0);_24=0;k.set(0, 0, 1);_34=0;c.set(0, 0, 0);_44=1;	return *this; }
	IC	SelfRef	mapXZY		()	{i.set(1, 0, 0);_14=0;j.set(0, 0, 1);_24=0;k.set(0, 1, 0);_34=0;c.set(0, 0, 0);_44=1;	return *this; }
	IC	SelfRef	mapYXZ		()	{i.set(0, 1, 0);_14=0;j.set(1, 0, 0);_24=0;k.set(0, 0, 1);_34=0;c.set(0, 0, 0);_44=1;	return *this; }
	IC	SelfRef	mapYZX		()	{i.set(0, 1, 0);_14=0;j.set(0, 0, 1);_24=0;k.set(1, 0, 0);_34=0;c.set(0, 0, 0);_44=1;	return *this; }
	IC	SelfRef	mapZXY		()	{i.set(0, 0, 1);_14=0;j.set(1, 0, 0);_24=0;k.set(0, 1, 0);_34=0;c.set(0, 0, 0);_44=1;	return *this; }
	IC	SelfRef	mapZYX		()	{i.set(0, 0, 1);_14=0;j.set(0, 1, 0);_24=0;k.set(1, 0, 0);_34=0;c.set(0, 0, 0);_44=1;	return *this; }

	IC	SelfRef	rotation	( const Tvector &axis, T Angle )	{
		T Cosine	= _cos(Angle);
		T Sine		= _sin(Angle);
		m [0][0] 	= axis.x * axis.x + ( 1 - axis.x * axis.x) * Cosine;
		m [0][1] 	= axis.x * axis.y * ( 1 - Cosine ) + axis.z * Sine;
		m [0][2] 	= axis.x * axis.z * ( 1 - Cosine ) - axis.y * Sine;
		m [0][3] 	= 0;
		m [1][0] 	= axis.x * axis.y * ( 1 - Cosine ) - axis.z * Sine;
		m [1][1] 	= axis.y * axis.y + ( 1 - axis.y * axis.y) * Cosine;
		m [1][2] 	= axis.y * axis.z * ( 1 - Cosine ) + axis.x * Sine;
		m [1][3] 	= 0;
		m [2][0] 	= axis.x * axis.z * ( 1 - Cosine ) + axis.y * Sine;
		m [2][1] 	= axis.y * axis.z * ( 1 - Cosine ) - axis.x * Sine;
		m [2][2] 	= axis.z * axis.z + ( 1 - axis.z * axis.z) * Cosine;
		m [2][3] 	= 0; m [3][0] = 0; m [3][1] = 0;
		m [3][2] 	= 0; m [3][3] = 1;
		return *this; 
	}

	// mirror X
	IC	SelfRef	mirrorX ()			{	
		identity();	m[0][0] = -1;	
		return *this; 
	}
	IC	SelfRef	mirrorX_over ()		{	
		m[0][0] = -1;	
		return *this; 
	}
	IC	SelfRef	mirrorX_add ()		{	
		m[0][0] *= -1;	
		return *this; 
	}

	// mirror Y
	IC	SelfRef	mirrorY ()			{	
		identity();	m [1][1] = -1;	
		return *this; 
	}
	IC	SelfRef	mirrorY_over ()		{	
		m[1][1] = -1;	
		return *this; 
	}
	IC	SelfRef	mirrorY_add ()		{	
		m[1][1] *= -1;	
		return *this; 
	}

	// mirror Z
	IC	SelfRef	mirrorZ ()			{	
		identity();		m [2][2] = -1;	
		return *this; 
	}
	IC	SelfRef	mirrorZ_over ()		{	
		m[2][2] = -1;	
		return *this; 
	}
	IC	SelfRef	mirrorZ_add ()		{	
		m[2][2] *= -1;	
		return *this; 
	}
	IC	SelfRef	mul( const Self &A, T v )	{
		m[0][0] = A.m [0][0] * v;	m[0][1] = A.m [0][1] * v;	m[0][2] = A.m [0][2] * v;	m[0][3] = A.m [0][3] * v;
		m[1][0] = A.m [1][0] * v;	m[1][1] = A.m [1][1] * v;	m[1][2] = A.m [1][2] * v;	m[1][3] = A.m [1][3] * v;
		m[2][0] = A.m [2][0] * v;	m[2][1] = A.m [2][1] * v;	m[2][2] = A.m [2][2] * v;	m[2][3] = A.m [2][3] * v;
		m[3][0] = A.m [3][0] * v;	m[3][1] = A.m [3][1] * v;	m[3][2] = A.m [3][2] * v;	m[3][3] = A.m [3][3] * v;
		return *this; 
	}
	IC	SelfRef	mul( T v )	{
		m[0][0] *= v;		m[0][1] *= v;		m[0][2] *= v;		m[0][3] *= v;
		m[1][0] *= v;		m[1][1] *= v;		m[1][2] *= v;		m[1][3] *= v;
		m[2][0] *= v;		m[2][1] *= v;		m[2][2] *= v;		m[2][3] *= v;
		m[3][0] *= v;		m[3][1] *= v;		m[3][2] *= v;		m[3][3] *= v;
		return *this; 
	}
	IC	SelfRef	div( const Self &A, T v )	{
		VERIFY(_abs(v)>0.000001f);
		return mul(A,1.0f/v);
	}
	IC	SelfRef	div( T v )					{
		VERIFY(_abs(v)>0.000001f);
		return mul(1.0f/v);
	}
	// fov
	IC	SelfRef	build_projection		(T fFOV, T fAspect, T fNearPlane, T fFarPlane)	{
		return build_projection_HAT		(tanf(fFOV/2.f),fAspect,fNearPlane,fFarPlane);
	}
	// half_fov-angle-tangent
	IC	SelfRef	build_projection_HAT	(T HAT, T fAspect, T fNearPlane, T fFarPlane) 
	{
		VERIFY( _abs(fFarPlane-fNearPlane) > EPS_S );
		VERIFY( _abs(HAT) > EPS_S );
		
		T cot	= T(1)/HAT;
		T w		= fAspect * cot;
		T h		= T(1)    * cot;
		T Q		= fFarPlane / ( fFarPlane - fNearPlane );
		
		_11		= w;	_12 = 0;	_13 = 0;			_14 = 0;
		_21		= 0;	_22	= h;	_23 = 0;			_24 = 0;
		_31		= 0;	_32 = 0;	_33 = Q;			_34 = 1.0f;
		_41		= 0;	_42 = 0;	_43	= -Q*fNearPlane;_44 = 0;
		return *this; 
	}
	IC	SelfRef	build_projection_ortho	(T w, T h, T zn, T zf)
	{
		_11	= T(2)/w;	_12 = 0;		_13 = 0;			_14 = 0;
		_21 = 0;		_22 = T(2)/h;	_23	= 0;			_24	= 0;
		_31 = 0;		_32 = 0;		_33	= T(1)/(zf-zn);	_34	= 0;
		_41 = 0;		_42 = 0;		_43	= zn/(zn-zf);	_44	= T(1);
		return *this; 
	}
	IC	SelfRef	build_camera(const Tvector &vFrom, const Tvector &vAt, const Tvector &vWorldUp) 
	{
		// Get the z basis vector3, which points straight ahead. This is the
		// difference from the eyepoint to the lookat point.
		Tvector vView;
		vView.sub		(vAt,vFrom).normalize();

		// Get the dot product, and calculate the projection of the z basis
		// vector3 onto the up vector3. The projection is the y basis vector3.
		T fDotProduct = vWorldUp.dotproduct( vView );

		Tvector vUp;
		vUp.mul	(vView, -fDotProduct).add(vWorldUp).normalize();

		// The x basis vector3 is found simply with the cross product of the y
		// and z basis vectors
		Tvector vRight;
		vRight.crossproduct( vUp, vView );

		// Start building the Device.mView. The first three rows contains the basis
		// vectors used to rotate the view to point at the lookat point
		_11 = vRight.x;  _12 = vUp.x;  _13 = vView.x;  _14 = 0.0f;
		_21 = vRight.y;  _22 = vUp.y;  _23 = vView.y;  _24 = 0.0f;
		_31 = vRight.z;  _32 = vUp.z;  _33 = vView.z;  _34 = 0.0f;

		// Do the translation values (rotations are still about the eyepoint)
		_41 = - vFrom.dotproduct(vRight);
		_42 = - vFrom.dotproduct( vUp  );
		_43 = - vFrom.dotproduct(vView );
		_44 = 1.0f;
		return *this; 
	}
	IC	SelfRef	build_camera_dir(const Tvector &vFrom, const Tvector &vView, const Tvector &vWorldUp) 
	{
		// Get the dot product, and calculate the projection of the z basis
		// vector3 onto the up vector3. The projection is the y basis vector3.
		T fDotProduct = vWorldUp.dotproduct( vView );

		Tvector vUp;
		vUp.mul	(vView, -fDotProduct).add(vWorldUp).normalize();

		// The x basis vector3 is found simply with the cross product of the y
		// and z basis vectors
		Tvector vRight;
		vRight.crossproduct( vUp, vView );

		// Start building the Device.mView. The first three rows contains the basis
		// vectors used to rotate the view to point at the lookat point
		_11 = vRight.x;  _12 = vUp.x;  _13 = vView.x;  _14 = 0.0f;
		_21 = vRight.y;  _22 = vUp.y;  _23 = vView.y;  _24 = 0.0f;
		_31 = vRight.z;  _32 = vUp.z;  _33 = vView.z;  _34 = 0.0f;

		// Do the translation values (rotations are still about the eyepoint)
		_41 = - vFrom.dotproduct(vRight);
		_42 = - vFrom.dotproduct( vUp  );
		_43 = - vFrom.dotproduct(vView );
		_44 = 1.0f;
		return *this; 
	}

	IC	SelfRef	inertion(const Self &mat, T v)
	{
		T iv = 1.f-v;
		for (int i=0; i<4; i++)
		{
			m[i][0] = m[i][0]*v + mat.m[i][0]*iv;
			m[i][1] = m[i][1]*v + mat.m[i][1]*iv;
			m[i][2] = m[i][2]*v + mat.m[i][2]*iv;
			m[i][3] = m[i][3]*v + mat.m[i][3]*iv;
		}
		return *this; 
	}
	ICF	void	transform_tiny		(Tvector &dest, const Tvector &v)	const // preferred to use
	{
		dest.x = v.x*_11 + v.y*_21 + v.z*_31 + _41;
		dest.y = v.x*_12 + v.y*_22 + v.z*_32 + _42;
		dest.z = v.x*_13 + v.y*_23 + v.z*_33 + _43;
	}
	ICF	void	transform_tiny32	(Fvector2 &dest, const Tvector &v)	const // preferred to use
	{
		dest.x = v.x*_11 + v.y*_21 + v.z*_31 + _41;
		dest.y = v.x*_12 + v.y*_22 + v.z*_32 + _42;
	}
	ICF	void	transform_tiny23	(Tvector &dest, const Fvector2 &v)	const // preferred to use
	{
		dest.x = v.x*_11 + v.y*_21 + _41;
		dest.y = v.x*_12 + v.y*_22 + _42;
		dest.z = v.x*_13 + v.y*_23 + _43;
	}
	ICF	void	transform_dir		(Tvector &dest, const Tvector &v)	const 	// preferred to use
	{
		dest.x = v.x*_11 + v.y*_21 + v.z*_31;
		dest.y = v.x*_12 + v.y*_22 + v.z*_32;
		dest.z = v.x*_13 + v.y*_23 + v.z*_33;
	}
	IC	void	transform			(Fvector4 &dest, const Tvector &v)	const 	// preferred to use
	{
		dest.w = v.x*_14 + v.y*_24 + v.z*_34 + _44;
		dest.x = (v.x*_11 + v.y*_21 + v.z*_31 + _41)/dest.w;
		dest.y = (v.x*_12 + v.y*_22 + v.z*_32 + _42)/dest.w;
		dest.z = (v.x*_13 + v.y*_23 + v.z*_33 + _43)/dest.w;
	}
	IC	void	transform			(Tvector &dest, const Tvector &v)	const 	// preferred to use
	{
		T iw	= 1.f/(v.x*_14 + v.y*_24 + v.z*_34 + _44);
		dest.x	= (v.x*_11 + v.y*_21 + v.z*_31 + _41)*iw;
		dest.y	= (v.x*_12 + v.y*_22 + v.z*_32 + _42)*iw;
		dest.z	= (v.x*_13 + v.y*_23 + v.z*_33 + _43)*iw;
	}

	IC	void	transform			(Fvector4 &dest, const Fvector4 &v)	const 	// preferred to use
	{
		dest.w = v.x*_14 + v.y*_24 + v.z*_34 + v.w*_44;
		dest.x = v.x*_11 + v.y*_21 + v.z*_31 + v.w*_41;
		dest.y = v.x*_12 + v.y*_22 + v.z*_32 + v.w*_42;
		dest.z = v.x*_13 + v.y*_23 + v.z*_33 + v.w*_43;
	}

	ICF	void	transform_tiny		(Tvector &v) const
	{
		Tvector			res;
		transform_tiny	(res,v);
		v.set			(res);
	}
	IC	void	transform			(Tvector &v) const
	{
		Tvector			res;
		transform		(res,v);
		v.set			(res);
	}
	ICF	void	transform_dir		(Tvector &v) const
	{
		Tvector			res;
		transform_dir	(res,v);
		v.set			(res);
	}
	ICF	SelfRef	setHPB	(T h, T p, T b)
	{
        T _ch, _cp, _cb, _sh, _sp, _sb, _cc, _cs, _sc, _ss;

        _sh = _sin(h); _ch = _cos(h);
        _sp = _sin(p); _cp = _cos(p);
        _sb = _sin(b); _cb = _cos(b);
        _cc = _ch*_cb; _cs = _ch*_sb; _sc = _sh*_cb; _ss = _sh*_sb;

        i.set(_cc-_sp*_ss,	-_cp*_sb,	_sp*_cs+_sc	);	_14_=0;
        j.set(_sp*_sc+_cs,	_cp*_cb, 	_ss-_sp*_cc	);	_24_=0;
        k.set(-_cp*_sh,    	_sp,		_cp*_ch		);	_34_=0;
        c.set(0,			0,			0			);  _44_=1;
		return *this; 
    }
	ICF SelfRef setHPB(Tvector const& hpb) { return setHPB(hpb.x, hpb.y, hpb.z); }

	IC	SelfRef	setXYZ	(T x, T y, T z)	{return setHPB(y,x,z);}
	IC	SelfRef	setXYZ	(Tvector const& xyz)	{return setHPB(xyz.y,xyz.x,xyz.z);}
	IC	SelfRef	setXYZi	(T x, T y, T z)	{return setHPB(-y,-x,-z);}
	IC	SelfRef	setXYZi	(Tvector const& xyz)	{return setHPB(-xyz.y,-xyz.x,-xyz.z);}

	IC void getHPB (T& h, T& p, T& b) const
	{
		T cy = _sqrt(j.y * j.y + i.y * i.y);
		bool stable = cy > 16.0f * type_epsilon(T);
	
		h = stable ? -atan2(k.x, k.z) : -atan2(-i.z, i.x);
		p = -atan2(-k.y, cy);
		b = stable ? -atan2(i.y, j.y) : T(0);
    }

	IC void getYPR(T& yaw, T& pitch, T& roll) const
	{
		_quaternion<T> quat; quat.set(*this);

		T sinr_cosp = 2.0f * (quat.w * quat.x + quat.y * quat.z);
		T cosr_cosp = 1.0f - 2.0f * (quat.x * quat.x + quat.y * quat.y);
		roll = atan2(sinr_cosp, cosr_cosp);

		T sinp = 2.0f * (quat.w * quat.y - quat.z * quat.x);

		if (fabs(sinp) >= 0.9999f)
		{
			pitch = copysign(PI_DIV_2, sinp);
			yaw = 2.0f * atan2(quat.x, quat.w);
			roll = T(0);
		}
		else
		{
			pitch = asin(sinp);
			T siny_cosp = 2.0f * (quat.w * quat.z + quat.x * quat.y);
			T cosy_cosp = 1.0f - 2.0f * (quat.y * quat.y + quat.z * quat.z);
			yaw = atan2(siny_cosp, cosy_cosp);
		}
	}


	IC	void	getHPB	(Tvector& hpb) const{getHPB(hpb.x,hpb.y,hpb.z);}
	IC	void	getXYZ	(T& x, T& y, T& z) const{getHPB(y,x,z);}
	IC	void	getXYZ	(Tvector& xyz) const{getXYZ(xyz.x,xyz.y,xyz.z);}
	IC	void	getXYZi	(T& x, T& y, T& z) const{getHPB(y,x,z);x*=-1.f;y*=-1.f;z*=-1.f;}
	IC	void	getXYZi	(Tvector& xyz) const{getXYZ(xyz.x,xyz.y,xyz.z);xyz.mul(-1.f);}

	IC SelfRef hud_to_world()
	{
		Device.hud_to_world(*this);
		return *this;
	}

	IC SelfRef world_to_hud()
	{
		Device.world_to_hud(*this);
		return *this;
	}
};

typedef		_matrix<float>	Fmatrix;
typedef		_matrix<double>	Dmatrix;

template <class T>
BOOL	_valid			(const _matrix<T>& m)		
{ 
	return 
		_valid(m.i) && _valid(m._14_)	&& 
		_valid(m.j) && _valid(m._24_)	&&
		_valid(m.k) && _valid(m._34_)	&&
		_valid(m.c) && _valid(m._44_)	
		;
}

extern XRCORE_API Fmatrix	Fidentity;
extern XRCORE_API Dmatrix	Didentity;

#endif
