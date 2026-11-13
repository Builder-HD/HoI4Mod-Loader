	// hoi4_loader_variables: red 0 1 1, green 0 1 0.3, blue 0 1 0.02, reach 4 36 18
	// The ground side of a fire line (fill = fire_edge, with line = fire_line on top): along the
	// inside of the area's edge the land is scorched and glows, the fire front ragged and moving.
	// Variables (shader_variables): red/green/blue the glow's colour (defaults: the area's colour, else
	// red-orange), reach how far the fire reaches inwards in map pixels.
	// Based on the owner's Shadertoy fire line, with the distance to the area's edge
	// (HL_EdgeDistance) in place of the distance to a fixed segment.
	static const float2 HL_FE_MAP = float2( 5632.0f, 2048.0f );  // map size in pixels

	float HL_FeHash( float2 p )
	{
		return frac( sin( dot( p, float2( 127.1f, 311.7f ) ) ) * 43758.5453123f );
	}

	float HL_FeNoise( float2 p )
	{
		float2 i = floor( p );
		float2 f = frac( p );
		f = f * f * ( 3.0f - 2.0f * f );
		return lerp( lerp( HL_FeHash( i ), HL_FeHash( i + float2( 1.0f, 0.0f ) ), f.x ),
		             lerp( HL_FeHash( i + float2( 0.0f, 1.0f ) ), HL_FeHash( i + float2( 1.0f, 1.0f ) ), f.x ), f.y );
	}

	float HL_FeFbm( float2 p )
	{
		float v = 0.0f;
		float a = 0.5f;
		for ( int i = 0; i < 4; ++i )
		{
			v += a * HL_FeNoise( p );
			p *= 2.05f;
			a *= 0.5f;
		}
		return v;
	}

	float3 HL_Fill_fire_edge( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )
	{
		float2 p = vUV * HL_FE_MAP;  // map pixels
		float t = vGlobalTime;

		// Distance to the edge, warped by moving noise so the front is ragged and flutters.
		float d = HL_EdgeDistance( Mask, vUV, reach );
		float2 warp = float2( HL_FeFbm( p * 0.08f + float2( t * 0.4f, 0.0f ) ),
		                      HL_FeFbm( p * 0.08f + float2( 0.0f, t * 0.4f ) + 15.0f ) ) - 0.5f;
		d = max( 0.0f, d + ( warp.x + warp.y ) * 7.0f );

		// Scorched zone along the edge.
		float scorch = 1.0f - smoothstep( 2.0f, reach - 2.0f, d );
		float3 ground = lerp( vColor, vColor * 0.2f, scorch * 0.85f );

		// Flickering fire core and glow (flames rising northwards: v decreases).
		float fireNoise = HL_FeFbm( p * 0.25f + float2( 0.0f, t * 1.5f ) );
		float core = 1.0f - smoothstep( 0.5f, 3.0f + fireNoise * 4.0f, d );
		float glow = 1.0f - smoothstep( 0.0f, reach * 0.8f, d );

		float3 fire = float3( red, green, blue ) * glow * 1.2f;
		fire += float3( 1.0f, 0.85f, 0.3f ) * core * 1.8f;
		return ground + fire;
	}
