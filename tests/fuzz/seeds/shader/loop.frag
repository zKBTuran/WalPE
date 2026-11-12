varying vec2 v_TexCoord;
uniform sampler2D g_Texture0;
uniform vec4 g_Texture0Resolution;
#define N 4
float f(float x) { return x * 2.0; }
void main() {
	vec4 s = vec4(0.0);
	for (int i = 0; i < N; i++)
		s += texSample2D(g_Texture0, v_TexCoord + vec2(float(i), 0.0) / g_Texture0Resolution.xy);
	int k = 3 % 2;
	float a[2];
	a[0] = f(s.r); a[1] = pow(s.g, 2);
	gl_FragColor = s / float(N) + a[k] * 0.0;
}
