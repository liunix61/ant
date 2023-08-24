
{
    fsinput.frag_coord = gl_FragCoord;
    fsinput.uv0 = uv_motion(v_texcoord0);

#ifndef MATERIAL_UNLIT
    fsinput.pos    = v_posWS;
#   ifdef WITH_NORMAL_ATTRIB
    fsinput.normal = v_normal;
#   endif //WITH_NORMAL_ATTRIB

#   ifdef WITH_TANGENT_ATTRIB
    fsinput.tangent = v_tangent;
#   endif //WITH_TANGENT_ATTRIB

#endif //MATERIAL_UNLIT

#ifdef WITH_COLOR_ATTRIB
    fsinput.color = v_color0;
#else //!WITH_COLOR_ATTRIB
    fsinput.color = vec4_splat(1.0);
#endif //WITH_COLOR_ATTRIB

#if defined(USING_LIGHTMAP)
    fsinput.uv1 = v_texcoord1;
#endif //USING_LIGHTMAP

#ifdef OUTPUT_USER_ATTR_0
    fsinput.user0 = v_texcoord2;
#endif

#ifdef OUTPUT_USER_ATTR_1
    fsinput.user1 = v_texcoord3;
#endif

#ifdef OUTPUT_USER_ATTR_2
    fsinput.user2 = v_texcoord4;
#endif

#ifdef OUTPUT_USER_ATTR_3
    fsinput.user3 = v_texcoord5;
#endif

#ifdef OUTPUT_USER_ATTR_4
    fsinput.user4 = v_texcoord6;
#endif
}