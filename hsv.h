// hsv.h by Bram Stolk

#ifndef HSV_H
#define HSV_H

static __inline__ void hsv_to_rgb
(
	float h, float s, float v,
	float *r, float *g, float *b 
)
{
	if( s == 0.0f ) 
	{
		// achromatic (grey)
		*r = *g = *b = v;
		return;
	}
	h *= 6.0f;			// sector 0 to 5
	int i = floor( h );
	float f = h - i;		// factorial part of h
	float p = v * ( 1.0f - s );
	float q = v * ( 1.0f - s * f );
	float t = v * ( 1.0f - s * ( 1.0f - f ) );
	switch( i ) 
	{
		case 0:
			*r = v;
			*g = t;
			*b = p;
			break;
		case 1:
			*r = q;
			*g = v;
			*b = p;
			break;
		case 2:
			*r = p;
			*g = v;
			*b = t;
			break;
		case 3:
			*r = p;
			*g = q;
			*b = v;
			break;
		case 4:
			*r = t;
			*g = p;
			*b = v;
			break;
		default:		// case 5:
			*r = v;
			*g = p;
			*b = q;
			break;
	}
}


static __inline__ uint32_t hsv_to_rgb24(float h, float s, float v)
{
	float red,grn,blu;
	hsv_to_rgb(h,s,v, &red,&grn,&blu);
	const uint8_t r = (uint8_t)( red * 255.99f);
	const uint8_t g = (uint8_t)( grn * 255.99f);
	const uint8_t b = (uint8_t)( blu * 255.99f);
	return (r<<16) | (g<<8) | (b<<0);
}


static __inline__ float hue_to_rgb
(
	float p,
	float q,
	float t
)
{
	if ( t < 0 ) t += 1;
	if ( t > 1 ) t -= 1;
	if ( t < 1/6.0f ) return p + (q - p) * 6.0f * t;
	if ( t < 1/2.0f ) return q;
	if ( t < 2/3.0f ) return p + (q - p) * (2.0f/3.0f - t) * 6.0f;
	return p;
}


static __inline__ void hsl_to_rgb
(
	float h, float s, float l,
	float* r, float* g, float* b
)
{
	if ( s == 0 )
	{
		*r = *g = *b = l; // achromatic
	}
	else
	{
		float q = l < 0.5f ? l * ( 1 + s ) : l + s - l * s;
		float p = 2 * l - q;
		*r = hue_to_rgb( p, q, h + 1/3.0f );
		*g = hue_to_rgb( p, q, h );
		*b = hue_to_rgb( p, q, h - 1/3.0f );
	}
}


static __inline__ float linear_to_srgb(float val)
{
	if (val <= 0.0031308f)
		return 12.92f * val;
	else
		return 1.055f * powf(val, 1.0f / 2.4f) - 0.055f;
}


static __inline__ float srgb_to_linear(float val)
{
	if (val < 0.04045f)
		return val * (1.0f / 12.92f);
	else
		return powf((val + 0.055f) * (1.0f / 1.055f), 2.4f);
}


#endif

