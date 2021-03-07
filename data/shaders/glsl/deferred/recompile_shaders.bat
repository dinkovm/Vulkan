glslangvalidator -V chit_floor.rchit -o chit_floor.rchit.spv --target-env spirv1.4
glslangvalidator -V chit_model.rchit -o chit_model.rchit.spv --target-env spirv1.4
glslangvalidator -V raygen.rgen -o raygen.rgen.spv --target-env spirv1.4
glslangvalidator -V rint_light.rint -o rint_light.rint.spv --target-env spirv1.4
glslangvalidator -V chit_light.rchit -o chit_light.rchit.spv --target-env spirv1.4
glslangvalidator -V miss.rmiss -o miss.rmiss.spv --target-env spirv1.4
glslangvalidator -V shadow.rmiss -o shadow.rmiss.spv --target-env spirv1.4