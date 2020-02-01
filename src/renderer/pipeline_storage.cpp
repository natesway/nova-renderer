#include "nova_renderer/pipeline_storage.hpp"

#include "nova_renderer/nova_renderer.hpp"
#include "nova_renderer/rhi/render_device.hpp"

#include "spirv_glsl.hpp"

using namespace spirv_cross;

namespace nova::renderer {
    RX_LOG("PipelineStorage", logger);

    using namespace mem;
    using namespace ntl;
    using namespace shaderpack;

    PipelineStorage::PipelineStorage(NovaRenderer& renderer, rx::memory::allocator* allocator)
        : renderer(renderer), device(renderer.get_engine()), allocator(allocator) {}

    rx::optional<renderer::Pipeline> PipelineStorage::get_pipeline(const rx::string& pipeline_name) const {
        if(const auto* pipeline = pipelines.find(pipeline_name)) {
            return *pipeline;

        } else {
            return rx::nullopt;
        }
    }

    bool PipelineStorage::create_pipeline(const PipelineCreateInfo& create_info) {
        const auto rp_create = renderer.get_renderpass_metadata(create_info.pass);
        if(!rp_create) {
            logger(rx::log::level::k_error,
                   "Pipeline %s wants to be rendered by renderpass %s, but that renderpass doesn't have any metadata",
                   create_info.name,
                   create_info.pass);
            return false;
        }

        Result<rhi::PipelineInterface*> pipeline_interface = create_pipeline_interface(create_info,
                                                                                       rp_create->data.texture_outputs,
                                                                                       rp_create->data.depth_texture);
        if(!pipeline_interface) {
            logger(rx::log::level::k_error,
                   "Pipeline %s has an invalid interface: %s",
                   create_info.name,
                   pipeline_interface.error.to_string());
            return false;
        }

        Result<PipelineReturn> pipeline_result = create_graphics_pipeline(*pipeline_interface, create_info);
        if(pipeline_result) {
            auto [pipeline, pipeline_metadata] = *pipeline_result;

            pipelines.insert(create_info.name, pipeline);
            pipeline_metadatas.insert(create_info.name, pipeline_metadata);

            return true;
        } else {
            logger(rx::log::level::k_error, "Could not create pipeline %s:%s", create_info.name, pipeline_result.error.to_string());

            return false;
        }
    }

    Result<PipelineReturn> PipelineStorage::create_graphics_pipeline(rhi::PipelineInterface* pipeline_interface,
                                                                     const PipelineCreateInfo& pipeline_create_info) const {
        renderer::Pipeline pipeline;
        PipelineMetadata metadata;

        metadata.data = pipeline_create_info;

        Result<rhi::Pipeline*> rhi_pipeline = device.create_pipeline(pipeline_interface, pipeline_create_info, allocator);
        if(rhi_pipeline) {
            pipeline.pipeline = *rhi_pipeline;
            pipeline.pipeline_interface = pipeline_interface;

        } else {
            NovaError error = NovaError{rx::string::format("Could not create pipeline %s", pipeline_create_info.name),
                                        allocator->create<NovaError>(rhi_pipeline.error)};
            return ntl::Result<PipelineReturn>(rx::utility::move(error));
        }

        return Result(PipelineReturn{pipeline, metadata});
    }

    Result<rhi::PipelineInterface*> PipelineStorage::create_pipeline_interface(
        const PipelineCreateInfo& pipeline_create_info,
        const rx::vector<TextureAttachmentInfo>& color_attachments,
        const rx::optional<TextureAttachmentInfo>& depth_texture) const {

        rx::map<rx::string, rhi::ResourceBindingDescription> bindings;

        get_shader_module_descriptors(pipeline_create_info.vertex_shader.source, rhi::ShaderStage::Vertex, bindings);

        if(pipeline_create_info.tessellation_control_shader) {
            get_shader_module_descriptors(pipeline_create_info.tessellation_control_shader->source,
                                          rhi::ShaderStage::TessellationControl,
                                          bindings);
        }
        if(pipeline_create_info.tessellation_evaluation_shader) {
            get_shader_module_descriptors(pipeline_create_info.tessellation_evaluation_shader->source,
                                          rhi::ShaderStage::TessellationEvaluation,
                                          bindings);
        }
        if(pipeline_create_info.geometry_shader) {
            get_shader_module_descriptors(pipeline_create_info.geometry_shader->source, rhi::ShaderStage::Geometry, bindings);
        }
        if(pipeline_create_info.fragment_shader) {
            get_shader_module_descriptors(pipeline_create_info.fragment_shader->source, rhi::ShaderStage::Fragment, bindings);
        }

        return device.create_pipeline_interface(bindings, color_attachments, depth_texture, allocator)
            .map([&](rhi::PipelineInterface* pipeline_interface) {
                pipeline_interface->vertex_fields = get_vertex_fields(pipeline_create_info.vertex_shader);
                return pipeline_interface;
            });
    }

    rhi::VertexFieldFormat to_rhi_vertex_format(const SPIRType& spirv_type) {
        switch(spirv_type.basetype) {
            case SPIRType::UInt:
                return rhi::VertexFieldFormat::Uint;

            case SPIRType::Float: {
                switch(spirv_type.vecsize) {
                    case 2:
                        return rhi::VertexFieldFormat::Float2;

                    case 3:
                        return rhi::VertexFieldFormat::Float3;

                    case 4:
                        return rhi::VertexFieldFormat::Float4;

                    default:
                        logger(rx::log::level::k_error, "Nova does not support float fields with %u vector elements", spirv_type.vecsize);
                        return rhi::VertexFieldFormat::Invalid;
                }
            };

            case SPIRType::Unknown:
                [[fallthrough]];
            case SPIRType::Void:
                [[fallthrough]];
            case SPIRType::Boolean:
                [[fallthrough]];
            case SPIRType::SByte:
                [[fallthrough]];
            case SPIRType::UByte:
                [[fallthrough]];
            case SPIRType::Short:
                [[fallthrough]];
            case SPIRType::UShort:
                [[fallthrough]];
            case SPIRType::Int:
                [[fallthrough]];
            case SPIRType::Int64:
                [[fallthrough]];
            case SPIRType::UInt64:
                [[fallthrough]];
            case SPIRType::AtomicCounter:
                [[fallthrough]];
            case SPIRType::Half:
                [[fallthrough]];
            case SPIRType::Double:
                [[fallthrough]];
            case SPIRType::Struct:
                [[fallthrough]];
            case SPIRType::Image:
                [[fallthrough]];
            case SPIRType::SampledImage:
                [[fallthrough]];
            case SPIRType::Sampler:
                [[fallthrough]];
            case SPIRType::AccelerationStructureNV:
                [[fallthrough]];
            case SPIRType::ControlPointArray:
                [[fallthrough]];
            case SPIRType::Char:
                [[fallthrough]];
            default:
                logger(rx::log::level::k_error, "Nova does not support vertex fields of type %u", spirv_type.basetype);
        }

        return {};
    }

    rx::vector<rhi::VertexField> PipelineStorage::get_vertex_fields(const ShaderSource& vertex_shader) const {
        const CompilerGLSL shader_compiler(vertex_shader.source.data(), vertex_shader.source.size());

        const auto& shader_vertex_fields = shader_compiler.get_shader_resources().stage_inputs;

        rx::vector<rhi::VertexField> vertex_fields;
        vertex_fields.reserve(shader_vertex_fields.size());

        for(const auto& spirv_field : shader_vertex_fields) {
            const auto& spirv_type = shader_compiler.get_type(spirv_field.base_type_id);
            const auto format = to_rhi_vertex_format(spirv_type);

            vertex_fields.emplace_back(spirv_field.name.c_str(), format);
        }

        return vertex_fields;
    }

    void PipelineStorage::get_shader_module_descriptors(const rx::vector<uint32_t>& spirv,
                                                        const rhi::ShaderStage shader_stage,
                                                        rx::map<rx::string, rhi::ResourceBindingDescription>& bindings) {
        const CompilerGLSL shader_compiler(spirv.data(), spirv.size());
        const ShaderResources resources = shader_compiler.get_shader_resources();

        for(const auto& resource : resources.separate_images) {
            logger(rx::log::level::k_verbose, "Found a image named %s", resource.name);
            add_resource_to_bindings(bindings, shader_stage, shader_compiler, resource, rhi::DescriptorType::Texture);
        }

        for(const auto& resource : resources.separate_samplers) {
            logger(rx::log::level::k_verbose, "Found a sampler named %s", resource.name);
            add_resource_to_bindings(bindings, shader_stage, shader_compiler, resource, rhi::DescriptorType::Sampler);
        }

        for(const auto& resource : resources.sampled_images) {
            logger(rx::log::level::k_verbose, "Found a sampled image resource named %s", resource.name);
            add_resource_to_bindings(bindings, shader_stage, shader_compiler, resource, rhi::DescriptorType::CombinedImageSampler);
        }

        for(const auto& resource : resources.uniform_buffers) {
            logger(rx::log::level::k_verbose, "Found a UBO resource named %s", resource.name);
            add_resource_to_bindings(bindings, shader_stage, shader_compiler, resource, rhi::DescriptorType::UniformBuffer);
        }

        for(const auto& resource : resources.storage_buffers) {
            logger(rx::log::level::k_verbose, "Found a SSBO resource named %s", resource.name);
            add_resource_to_bindings(bindings, shader_stage, shader_compiler, resource, rhi::DescriptorType::StorageBuffer);
        }
    }

    void PipelineStorage::add_resource_to_bindings(rx::map<rx::string, rhi::ResourceBindingDescription>& bindings,
                                                   const rhi::ShaderStage shader_stage,
                                                   const CompilerGLSL& shader_compiler,
                                                   const spirv_cross::Resource& resource,
                                                   const rhi::DescriptorType type) {
        const uint32_t set = shader_compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
        const uint32_t binding = shader_compiler.get_decoration(resource.id, spv::DecorationBinding);

        rhi::ResourceBindingDescription new_binding = {};
        new_binding.set = set;
        new_binding.binding = binding;
        new_binding.type = type;
        new_binding.count = 1;
        new_binding.stages = shader_stage;

        const SPIRType& type_information = shader_compiler.get_type(resource.type_id);
        if(!type_information.array.empty()) {
            new_binding.count = type_information.array[0];
            // All arrays are unbounded until I figure out how to use SPIRV-Cross to detect unbounded arrays
            new_binding.is_unbounded = true;
        }

        const rx::string& resource_name = resource.name.c_str();

        if(auto* binding = bindings.find(resource_name)) {
            // Existing binding. Is it the same as our binding?
            rhi::ResourceBindingDescription& existing_binding = *binding;
            if(existing_binding != new_binding) {
                // They have two different bindings with the same name. Not allowed
                logger(rx::log::level::k_error,
                       "You have two different uniforms named %s in different shader stages. This is not allowed. Use unique names",
                       resource.name);

            } else {
                // Same binding, probably at different stages - let's fix that
                existing_binding.stages |= shader_stage;
            }

        } else {
            // Totally new binding!
            bindings.insert(resource_name, new_binding);
        }
    }
} // namespace nova::renderer
