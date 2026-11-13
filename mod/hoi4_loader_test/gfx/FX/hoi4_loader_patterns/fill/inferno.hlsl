	// Burning, smoking, devastated land (fill = inferno). The area's colour is the flames'
	// middle colour (orange is a good start; red gives a darker blaze, yellow a hotter one).
	// Map-anchored (vUV 0..1 over the whole map, v growing southwards), animated with vGlobalTime:
	// smoke drifts north-east, flames flicker upwards, embers glow and fade. The heat varies over the
	// map at about province size, so neighbouring provinces burn differently: some blaze, some only
	// smoulder under the smoke.
	static const float2 HL_INF_MAP = float2( 5632.0f, 2048.0f );  // map size in pixels
	static const float HL_INF_SCALE = 24.0f;                      // map pixels per noise unit

	float HL_InfHash( float2 p )
	{
		return frac( sin( dot( p, float2( 127.1f, 311.7f ) ) ) * 43758.5453123f );
	}

	float HL_InfNoise( float2 p )  // value noise, smooth
	{
		float2 i = floor( p );
		float2 f = frac( p );
		float2 u = f * f * ( 3.0f - 2.0f * f );
		float a = HL_InfHash( i );
		float b = HL_InfHash( i + float2( 1.0f, 0.0f ) );
		float c = HL_InfHash( i + float2( 0.0f, 1.0f ) );
		float d = HL_InfHash( i + float2( 1.0f, 1.0f ) );
		return lerp( lerp( a, b, u.x ), lerp( c, d, u.x ), u.y );
	}

	float HL_InfFbm( float2 p )  // four octaves
	{
		float v = 0.0f;
		float amp = 0.5f;
		for ( int k = 0; k < 4; ++k )
		{
			v += amp * HL_InfNoise( p );
			p = p * 2.03f + float2( 17.1f, 9.3f );
			amp *= 0.5f;
		}
		return v;
	}

	float3 HL_InfFireRamp( float t, float3 vMid )  // 0 cold .. 1 white hot
	{
		float3 c = lerp( float3( 0.05f, 0.0f, 0.0f ), float3( 0.55f, 0.05f, 0.0f ), saturate( t * 3.0f ) );
		c = lerp( c, vMid, saturate( t * 3.0f - 1.0f ) );
		return lerp( c, float3( 1.0f, 0.95f, 0.7f ), saturate( t * 3.0f - 2.0f ) );
	}

	float3 HL_Fill_inferno( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )
	{
		float2 p = vUV * HL_INF_MAP / HL_INF_SCALE;
		float t = vGlobalTime;

		// Heat: how hard this spot burns, drifting slowly (blazing patches and smouldering ones).
		float heat = smoothstep( 0.35f, 0.75f, HL_InfFbm( p * 0.18f + float2( t * 0.01f, 0.0f ) ) );

		// Devastation: the ground charred, desaturated and darkened, with ash streaks.
		float grey = dot( vColor, float3( 0.3f, 0.59f, 0.11f ) );
		float ash = HL_InfFbm( p * 1.7f );
		float3 ground = lerp( float3( grey, grey, grey ) * float3( 0.45f, 0.38f, 0.32f ), float3( 0.08f, 0.07f, 0.06f ), 0.55f + 0.3f * ash );

		// Flames: noise rising northwards (v decreases), flickering; strongest where the heat is.
		float flame = HL_InfFbm( p * 1.3f + float2( sin( t * 0.7f + p.y * 0.3f ) * 0.15f, t * 1.1f ) );
		flame = saturate( ( flame - 0.55f + heat * 0.35f ) * 3.0f ) * heat;
		float flicker = 0.8f + 0.2f * sin( t * 9.0f + HL_InfHash( floor( p * 2.0f ) ) * 6.28f );
		float3 fire = HL_InfFireRamp( flame * flicker, vMask.rgb );

		// Embers: sparse sparks that glow up and fade.
		float2 cell = floor( p * 3.0f );
		float seed = HL_InfHash( cell );
		float spark = step( 0.93f, seed ) * saturate( sin( t * ( 1.5f + seed * 3.0f ) + seed * 40.0f ) );
		float2 cellUv = frac( p * 3.0f ) - 0.5f;
		spark *= saturate( 1.0f - length( cellUv ) * 4.0f ) * ( 0.3f + heat );

		// Smoke: big slow billows drifting north-east, thickest where it has burnt out.
		float smoke = HL_InfFbm( p * 0.45f + float2( -t * 0.06f, t * 0.12f ) );
		smoke = smoothstep( 0.4f, 0.85f, smoke ) * ( 0.75f - 0.45f * flame );
		float3 smokeColor = lerp( float3( 0.18f, 0.17f, 0.16f ), float3( 0.42f, 0.40f, 0.38f ), HL_InfNoise( p * 0.9f - t * 0.05f ) );

		float3 c = ground;
		c = lerp( c, fire, saturate( flame * 1.4f ) );
		c += float3( 1.0f, 0.55f, 0.15f ) * spark;
		c += float3( 0.35f, 0.08f, 0.0f ) * heat * 0.25f;  // red glow under it all
		c = lerp( c, smokeColor, smoke * 0.7f );
		return c;
	}
