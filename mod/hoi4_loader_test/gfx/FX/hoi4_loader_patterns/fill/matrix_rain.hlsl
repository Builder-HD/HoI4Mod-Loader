	// Falling "digital rain" in the area's colour (fill = matrix_rain). Map-anchored: the
	// grid is measured over the whole map (vUV 0..1), so the rain stays on the ground when the camera
	// moves. Tune: HL_RAIN_CELLS (character cells over the map), HL_RAIN_DROPS (drops per map height).
	// About 8 map pixels per character (first test: 4 px read as checkered noise, not characters).
	static const float2 HL_RAIN_CELLS = float2( 700.0f, 255.0f );  // about 2.75:1, the map's shape
	static const float HL_RAIN_DROPS = 40.0f;

	float HL_RainHash( float2 p )
	{
		return frac( sin( dot( p, float2( 127.1f, 311.7f ) ) ) * 43758.5453123f );
	}

	float HL_RainGlyph( float2 uv, float seed )
	{
		if ( uv.x < 0.15f || uv.x > 0.85f || uv.y < 0.1f || uv.y > 0.9f ) return 0.0f;
		return step( 0.5f, HL_RainHash( floor( uv * 5.0f ) + floor( seed * 20.0f ) ) );
	}

	float3 HL_Fill_matrix_rain( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )
	{
		float2 cellId = floor( vUV * HL_RAIN_CELLS );
		float2 cellUv = frac( vUV * HL_RAIN_CELLS );
		float colSpeed = HL_RainHash( float2( cellId.x, 1.0f ) ) * 0.5f + 0.3f;
		float colOffset = HL_RainHash( float2( cellId.x, 2.0f ) ) * 10.0f;
		// The map's v grows southwards (first in-game test: with +vUV.y the rain rose), so -vUV.y
		// makes the heads move down with the trail above them.
		float drop = frac( -vUV.y * HL_RAIN_DROPS + vGlobalTime * colSpeed + colOffset );
		float trail = pow( 1.0f - drop, 4.0f );
		float head = 1.0f - smoothstep( 0.0f, 0.05f, drop );
		float glyph = HL_RainGlyph( cellUv, HL_RainHash( cellId + floor( vGlobalTime * 8.0f ) ) );
		float3 rain = lerp( vMask.rgb, float3( 0.85f, 1.0f, 0.9f ), head ) * glyph * ( trail + head );
		return vColor * 0.35f + rain * 1.3f;  // a darker ground so the green reads
	}
