#version 450

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_texcoord;

layout(std140, binding = 0) uniform VertexUniforms
{
    vec4 u_transform;
};

void main()
{
    gl_Position = vec4((a_position.x * u_transform.x) + u_transform.z,
                       (a_position.y * u_transform.y) + u_transform.w,
                       0.0,
                       1.0);
    v_color = a_color;
    v_texcoord = a_texcoord;
}
