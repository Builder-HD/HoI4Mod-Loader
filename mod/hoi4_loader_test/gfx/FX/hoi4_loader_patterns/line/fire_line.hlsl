		// The white-hot core of a fire line (line = fire_line, with fill = fire_edge beneath): the
		// border strip burns, its width and brightness flickering along the line. The area's colour
		// tints the flame's edges. vUV.x runs across the strip (0..1), vUV.y along it.
		float HL_FlHash( float2 p )
		{
			return frac( sin( dot( p, float2( 127.1f, 311.7f ) ) ) * 43758.5453123f );
		}

		float HL_FlNoise( float2 p )
		{
			float2 i = floor( p );
			float2 f = frac( p );
			f = f * f * ( 3.0f - 2.0f * f );
			return lerp( lerp( HL_FlHash( i ), HL_FlHash( i + float2( 1.0f, 0.0f ) ), f.x ),
			             lerp( HL_FlHash( i + float2( 0.0f, 1.0f ) ), HL_FlHash( i + float2( 1.0f, 1.0f ) ), f.x ), f.y );
		}

		float4 HL_Line_fire_line( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos )
		{
			float t = vGlobalTime;
			float along = vUV.y * BORDER_TILE * 4.0f;
			float n = HL_FlNoise( float2( along * 3.0f - t * 2.0f, t * 1.3f ) ) * 0.6f
			        + HL_FlNoise( float2( along * 9.0f + t * 4.0f, 3.0f ) ) * 0.4f;
			float across = abs( vUV.x - 0.5f ) * 2.0f;  // 0 at the centre, 1 at the sides
			float core = 1.0f - smoothstep( 0.15f + n * 0.5f, 0.95f, across );
			float3 c = lerp( vAreaColor, float3( 1.0f, 0.9f, 0.55f ), saturate( core * 1.5f - 0.3f ) );
			float flicker = 0.75f + 0.25f * sin( t * 13.0f + along * 5.0f );
			return float4( c * ( 1.2f + n ) * flicker, saturate( core ) * saturate( vColor.a ) );
		}
