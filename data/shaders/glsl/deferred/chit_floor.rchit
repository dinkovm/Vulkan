#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable

struct Payload
{
	vec4 albedo;
	vec3 hitPos;
	vec3 hitNormal;
	float reflectance;
	int material;
};

layout(location = 0) rayPayloadInEXT Payload payload;
//layout(location = 2) rayPayloadEXT bool shadowed;
hitAttributeEXT vec3 attribs;

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;
layout(binding = 2, set = 0) uniform UBO 
{
	mat4 viewInverse;
	mat4 projInverse;
	vec4 lightPos;
	int vertexSize;
} ubo;
layout(binding = 7, set = 0) buffer Vertices { vec4 v[]; } vertices;
layout(binding = 8, set = 0) buffer Indices { uint i[]; } indices;
layout (binding = 9) uniform sampler2D samplerColor;
layout (binding = 10) uniform sampler2D samplerNormalMap;

struct Vertex
{
  vec3 pos;
  vec3 normal;
  vec2 uv;
  vec4 color;
  vec3 tangent;
  vec4 _pad1;
 };

Vertex unpack(uint index)
{
	// Unpack the vertices from the SSBO using the glTF vertex structure
	// The multiplier is the size of the vertex divided by four float components (=16 bytes)
	const int m = ubo.vertexSize / 16;

	vec4 d0 = vertices.v[m * index + 0]; // pos and normal.x
	vec4 d1 = vertices.v[m * index + 1]; // normal.yz and uv
	vec4 d2 = vertices.v[m * index + 2]; // color
	//vec4 d3 = vertices.v[m * index + 3]; // joint0
	//vec4 d4 = vertices.v[m * index + 4]; // weight0
	vec4 d5 = vertices.v[m * index + 5]; // tangent

	Vertex v;
	v.pos = d0.xyz;
	v.normal = vec3(d0.w, d1.x, d1.y);
	v.uv = vec2(d1.z, d1.w);
	v.color = vec4(d2.x, d2.y, d2.z, 1.0);
  v.tangent = d5.xyz;
  
	return v;
}

void main()
{
	ivec3 index = ivec3(indices.i[3 * gl_PrimitiveID], indices.i[3 * gl_PrimitiveID + 1], indices.i[3 * gl_PrimitiveID + 2]);

	Vertex v0 = unpack(index.x);
	Vertex v1 = unpack(index.y);
	Vertex v2 = unpack(index.z);

	// Interpolate
	const vec3 barycentricCoords = vec3(1.0f - attribs.x - attribs.y, attribs.x, attribs.y);
	vec3 N = normalize(v0.normal * barycentricCoords.x + v1.normal * barycentricCoords.y + v2.normal * barycentricCoords.z);
	vec3 T = normalize(v0.tangent * barycentricCoords.x + v1.tangent * barycentricCoords.y + v2.tangent * barycentricCoords.z);
	vec3 B = cross(N, T);
	mat3 TBN = mat3(T, B, N);
  
	vec2 uv = v0.uv * barycentricCoords.x + v1.uv * barycentricCoords.y + v2.uv * barycentricCoords.z;
	vec4 albedo = texture(samplerColor, uv);
	vec3 normal = TBN * normalize(texture(samplerNormalMap, uv).xyz * 2.0 - vec3(1.0));
	//vec3 fragPos = ((v0.pos * barycentricCoords.x + v1.pos * barycentricCoords.y + v2.pos * barycentricCoords.z) * gl_ObjectToWorldEXT).xyz;
        vec3 fragPos = gl_WorldRayOriginEXT + (gl_HitTEXT * gl_ObjectRayDirectionEXT);

	// Basic lighting
	//vec3 lightVector = normalize(ubo.lightPos.xyz);
	//float dot_product = max(dot(lightVector, normal), 0.2);
	//hitValue = albedo.rgb * dot_product;

	payload.albedo = albedo;
	payload.hitPos = fragPos;
	payload.hitNormal = normal;
	payload.reflectance = 1.0f;
	payload.material = 0;

	// Shadow casting
	//float tmin = 0.001;
	//float tmax = 10000.0;
	//vec3 origin = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
	//shadowed = true;  
	// Trace shadow ray and offset indices to match shadow hit/miss shader group indices
	//traceRayEXT(topLevelAS, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT, 0xFF, 1, 0, 1, origin, tmin, lightVector, tmax, 2);
	//if (shadowed) {
		//hitValue *= 0.3;
	//}
}
