		// hoi4_loader_variables: twist 1 12 3.8, speed 0 12 4, radius 0.1 0.45 0.3
		// A turning DNA double helix running along the border (line = dna_helix). Based on the owner's
		// Shadertoy helix: the border strip gives the coordinates its line maths computed (vUV.y along
		// the line, vUV.x across it), and an outline has no ends, so the end fade is gone. Variables
		// (shader_variables): twist (how tightly it winds), speed (turning), radius (strand swing in
		// strip widths). They take the line's three colour channels, so the strands are cyan and magenta; the strands shade darker and thinner when they turn
		// away, the front one covers the back one, rungs join them. It lives inside the game's border
		// strip, so it reads best zoomed in.

		float4 HL_Line_dna_helix( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos )
		{
			float across = vUV.x - 0.5f;
			float phase = vUV.y * BORDER_TILE * twist - vGlobalTime * speed;

			// The two strands: sideways position from sin, depth (towards the camera) from cos.
			float pos1 = sin( phase ) * radius;
			float depth1 = cos( phase );
			float pos2 = -pos1;
			float depth2 = -depth1;
			float near1 = depth1 * 0.5f + 0.5f;
			float near2 = depth2 * 0.5f + 0.5f;

			float thick1 = lerp( 0.05f, 0.13f, near1 );
			float thick2 = lerp( 0.05f, 0.13f, near2 );
			float strand1 = 1.0f - smoothstep( thick1 * 0.3f, thick1, abs( across - pos1 ) );
			float strand2 = 1.0f - smoothstep( thick2 * 0.3f, thick2, abs( across - pos2 ) );

			float3 color1 = float3( 0.1f, 0.85f, 1.0f ) * lerp( 0.35f, 1.3f, near1 );
			float3 color2 = float3( 1.0f, 0.25f, 0.6f ) * lerp( 0.35f, 1.3f, near2 );

			// Rungs between the strands.
			float rungGrid = frac( phase / 3.14159f * 2.0f );
			float isRung = 1.0f - smoothstep( 0.05f, 0.15f, abs( rungGrid - 0.5f ) );
			float minP = min( pos1, pos2 );
			float maxP = max( pos1, pos2 );
			float between = smoothstep( minP, minP + 0.03f, across ) * ( 1.0f - smoothstep( maxP - 0.03f, maxP, across ) );
			float rungMask = isRung * between * ( 1.0f - max( strand1, strand2 ) );
			float3 helix = float3( 0.8f, 0.9f, 1.0f ) * rungMask * 0.35f;

			// Depth order: the nearer strand is drawn last.
			if ( depth1 > depth2 )
			{
				helix = lerp( helix, color2, strand2 );
				helix = lerp( helix, color1, strand1 );
			}
			else
			{
				helix = lerp( helix, color1, strand1 );
				helix = lerp( helix, color2, strand2 );
			}

			// Soft glow around the strands.
			float glow1 = exp( -abs( across - pos1 ) * 6.75f ) * ( depth1 * 0.3f + 0.5f );
			float glow2 = exp( -abs( across - pos2 ) * 6.75f ) * ( depth2 * 0.3f + 0.5f );
			helix += float3( 0.1f, 0.85f, 1.0f ) * glow1 * 0.4f + float3( 1.0f, 0.25f, 0.6f ) * glow2 * 0.4f;

			float alpha = saturate( max( max( strand1, strand2 ), max( rungMask * 0.6f, ( glow1 + glow2 ) * 0.3f ) ) );
			return float4( helix, alpha * saturate( vColor.a ) );
		}
