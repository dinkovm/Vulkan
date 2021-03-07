#version 460
#extension GL_EXT_ray_tracing : require

struct Payload
{
	vec3 hitColor;
	vec3 hitPos;
	vec3 hitNormal;
	float reflectance;
	int material;
};

layout(location = 0) rayPayloadInEXT Payload payload;

void main()
{
	payload.hitColor = vec3(0);
	payload.hitPos = gl_WorldRayOriginEXT + (gl_RayTmaxEXT * gl_WorldRayDirectionEXT);
	payload.hitNormal = -1.0f * gl_WorldRayDirectionEXT;
	payload.reflectance = 1.0f;
	payload.material = 100;
}