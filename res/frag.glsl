#version 450 core

layout(binding = 2) uniform Ubo_Camera {
    mat4 projection;
    mat4 view;
}
ubo_camera;

layout(binding = 3) uniform sampler2D diffuse_color_map;
layout(binding = 4) uniform sampler2D emissive_color_map;
layout(binding = 5) uniform sampler2D normal_color_map;
layout(binding = 6) uniform sampler2D specular_color_map;

layout(location = 0) in vec4 in_col;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_pos;

layout(location = 0) out vec4 out_color;

// Light source
vec3 light_point = vec3(3, -10, 20) * 3;
float ambience = 0;

void main() {
    vec3 diffuse_color = texture(diffuse_color_map, in_uv).rgb;
    float specularity = texture(specular_color_map, in_uv).r;
    vec3 normal = texture(normal_color_map, in_uv).rgb;
    vec3 light_dir = light_point - in_pos;
    float distance = pow(length(light_dir), 2);
    light_dir = normalize(light_dir);
    float lambertian = max(dot(light_dir, normal), 0.0);
    float specular_brightness = 0.0;

    if (lambertian > 0.0) {
        // Draw vector from <0,0,0> (the view pos) to the vertex coordinates.
        vec3 view_dir = normalize(-in_pos);
        vec3 half_dir = normalize(light_dir + view_dir);
        float spec_angle = max(dot(half_dir, normal), 0.0);
        specular_brightness = pow(spec_angle, specularity);
    }
    out_color = vec4(diffuse_color * lambertian * specular_brightness, 1);
}
