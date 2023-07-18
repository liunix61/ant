highp   vec3 a_position   : POSITION;
mediump vec3 a_normal     : NORMAL;
mediump vec4 a_tangent	  : TANGENT;
mediump ivec4 a_indices	  : BLENDINDICES;
mediump vec4 a_weight	  : BLENDWEIGHT;
lowp    vec4 a_color0     : COLOR0;
mediump vec2 a_texcoord0  : TEXCOORD0;
mediump vec2 a_texcoord1  : TEXCOORD1;
mediump vec4 a_texcoord2  : TEXCOORD2;
mediump vec4 a_texcoord3  : TEXCOORD3;
mediump vec4 a_texcoord4  : TEXCOORD4;
mediump vec4 i_data2      : TEXCOORD5;
mediump vec4 i_data1      : TEXCOORD6;
mediump vec4 i_data0      : TEXCOORD7;

mediump vec2 v_texcoord0  : TEXCOORD0;
mediump vec2 v_texcoord1  : TEXCOORD1;
highp   vec4 v_posWS      : TEXCOORD2;
mediump vec3 v_normal     : TEXCOORD3;
mediump vec3 v_tangent    : TEXCOORD4;
mediump vec4 v_color0     : TEXCOORD5;
flat    vec4 v_texcoord2  : TEXCOORD6 = vec4(0.0, 0.0, 0.0, 0.0);
mediump vec4 v_texcoord3  : TEXCOORD7;
mediump vec4 v_texcoord4  : TEXCOORD8;
mediump vec4 v_texcoord5  : TEXCOORD9;
mediump vec4 v_texcoord6  : TEXCOORD10;