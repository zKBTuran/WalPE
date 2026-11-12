#include "common.h"
// [COMBO] {"material":"mode","combo":"MODE","type":"options","default":1}
varying vec4 v_TexCoord;
uniform float g_Time;
uniform sampler2D g_Texture0; // {"hidden":true}
uniform sampler2D g_Texture1; // {"material":"mask","combo":"MASK","default":"util/white"}
uniform float g_Speed; // {"material":"speed","default":2,"range":[0,10]}
uniform vec3 g_Tint; // {"material":"tint","type":"color","default":"1 0.5 0"}
void main() {
	vec4 c = texSample2D(g_Texture0, v_TexCoord.xy + frac(g_Time * g_Speed) * 0.01);
#if MASK
	c.a *= texSample2D(g_Texture1, v_TexCoord.zw).r;
#endif
	gl_FragColor = vec4(mix(c.rgb, g_Tint, 0.5), c.a);
}
//@vertex
uniform mat4 g_ModelViewProjectionMatrix;
attribute vec3 a_Position;
attribute vec2 a_TexCoord;
varying vec4 v_TexCoord;
void main() {
	gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
	v_TexCoord = a_TexCoord.xyxy;
}
