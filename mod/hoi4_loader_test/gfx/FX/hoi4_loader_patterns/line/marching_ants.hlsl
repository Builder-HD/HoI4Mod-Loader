		// Moving dashes along the outline, the area's colour alternating with black
		// (line = marching_ants). vUV.y runs along the border in the units the game's border
		// texture tiles in (BORDER_TILE); HL_ANTS_DASHES sets the dashes per texture repeat.
		static const float HL_ANTS_DASHES = 2.0f;
		static const float HL_ANTS_SPEED = 1.0f;

		float4 HL_Line_marching_ants( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos )
		{
			float fDash = step( 0.5f, frac( vUV.y * BORDER_TILE * HL_ANTS_DASHES - vGlobalTime * HL_ANTS_SPEED ) );
			float fEdge = 1.0f - smoothstep( 0.35f, 0.5f, abs( vUV.x - 0.5f ) );  // soft sides across the line
			float3 c = lerp( float3( 0.0f, 0.0f, 0.0f ), vAreaColor, fDash );
			return float4( c, max( vColor.a, fEdge ) );
		}
