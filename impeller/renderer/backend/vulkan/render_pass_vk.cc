// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/render_pass_vk.h"

#include "fml/logging.h"
#include "impeller/base/validation.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/device_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/formats_vk.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"

namespace impeller {

RenderPassVK::RenderPassVK(std::weak_ptr<const Context> context,
                           vk::Device device,
                           RenderTarget target,
                           vk::CommandPool command_pool,
                           vk::CommandBuffer command_buffer,
                           vk::RenderPass render_pass)
    : RenderPass(context, target),
      command_pool_(command_pool),
      command_buffer_(command_buffer),
      render_pass_(render_pass) {
  is_valid_ = true;
}

RenderPassVK::~RenderPassVK() = default;

bool RenderPassVK::IsValid() const {
  return is_valid_;
}

void RenderPassVK::OnSetLabel(std::string label) {
  label_ = std::move(label);
}

bool RenderPassVK::OnEncodeCommands(const Context& context) const {
  if (!IsValid()) {
    return false;
  }
  if (commands_.empty()) {
    return true;
  }
  const auto& render_target = GetRenderTarget();
  if (!render_target.HasColorAttachment(0u)) {
    return false;
  }

  vk::CommandBufferBeginInfo begin_info;
  auto res = command_buffer_.begin(begin_info);
  if (res != vk::Result::eSuccess) {
    VALIDATION_LOG << "Failed to begin command buffer: " << vk::to_string(res);
    return false;
  }

  const auto& color0 = render_target.GetColorAttachments().at(0u);
  const auto& depth0 = render_target.GetDepthAttachment();
  const auto& stencil0 = render_target.GetStencilAttachment();

  auto& wrapped_texture = TextureVK::Cast(*color0.texture);
  FML_CHECK(wrapped_texture.IsWrapped());

  auto texture_info = wrapped_texture.GetTextureInfo()->wrapped_texture;
  vk::UniqueFramebuffer framebuffer = CreateFrameBuffer(texture_info);

  vk::ClearValue clear_value;
  clear_value.color =
      vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0, 0.0f});

  std::array<vk::ImageView, 1> fbo_attachments = {
      texture_info.swapchain_image->GetImageView(),
  };

  const auto& image_size = texture_info.swapchain_image->GetSize();
  vk::Rect2D render_area =
      vk::Rect2D()
          .setOffset(vk::Offset2D(0, 0))
          .setExtent(vk::Extent2D(image_size.width, image_size.height));
  vk::RenderPassBeginInfo rp_begin_info = vk::RenderPassBeginInfo()
                                              .setRenderPass(render_pass_)
                                              .setFramebuffer(*framebuffer)
                                              .setRenderArea(render_area)
                                              .setClearValues(clear_value);

  command_buffer_.beginRenderPass(rp_begin_info, vk::SubpassContents::eInline);

  for (const auto& command : commands_) {
    if (command.index_count == 0u) {
      continue;
    }

    if (command.instance_count == 0u) {
      continue;
    }

    if (!command.pipeline) {
      continue;
    }

    if (!EncodeCommand(context, command)) {
      return false;
    }
  }

  command_buffer_.endRenderPass();
  res = command_buffer_.end();
  if (res != vk::Result::eSuccess) {
    VALIDATION_LOG << "Failed to end command buffer: " << vk::to_string(res);
    return false;
  }

  return true;
}

bool RenderPassVK::EncodeCommand(const Context& context,
                                 const Command& command) const {
  SetViewportAndScissor(command);

  auto& pipeline_vk = PipelineVK::Cast(*command.pipeline);
  PipelineCreateInfoVK* pipeline_create_info = pipeline_vk.GetCreateInfo();

  if (!AllocateAndBindDescriptorSets(context, command, pipeline_create_info)) {
    return false;
  }

  command_buffer_.bindPipeline(vk::PipelineBindPoint::eGraphics,
                               pipeline_create_info->GetVKPipeline());

  auto vertex_buffer_view = command.GetVertexBuffer();
  auto index_buffer_view = command.index_buffer;

  if (!vertex_buffer_view || !index_buffer_view) {
    return false;
  }

  auto& allocator = *context.GetResourceAllocator();
  const auto& pipeline_desc = command.pipeline->GetDescriptor();

  auto vertex_buffer = vertex_buffer_view.buffer->GetDeviceBuffer(allocator);
  auto index_buffer = index_buffer_view.buffer->GetDeviceBuffer(allocator);

  if (!vertex_buffer || !index_buffer) {
    VALIDATION_LOG << "Failed to acquire device buffers"
                   << " for vertex and index buffer views";
    return false;
  }

  // bind vertex buffer
  auto vertex_buffer_handle =
      DeviceBufferVK::Cast(*vertex_buffer).GetVKBufferHandle();
  vk::Buffer vertex_buffers[] = {vertex_buffer_handle};
  vk::DeviceSize vertex_buffer_offsets[] = {vertex_buffer_view.range.offset};
  command_buffer_.bindVertexBuffers(0, 1, vertex_buffers,
                                    vertex_buffer_offsets);

  // index buffer
  auto index_buffer_handle =
      DeviceBufferVK::Cast(*index_buffer).GetVKBufferHandle();
  command_buffer_.bindIndexBuffer(index_buffer_handle,
                                  index_buffer_view.range.offset,
                                  ToVKIndexType(command.index_type));

  // execute draw
  command_buffer_.drawIndexed(command.index_count, command.instance_count, 0, 0,
                              0);
  return true;
}

bool RenderPassVK::AllocateAndBindDescriptorSets(
    const Context& context,
    const Command& command,
    PipelineCreateInfoVK* pipeline_create_info) const {
  auto& allocator = *context.GetResourceAllocator();
  vk::PipelineLayout pipeline_layout =
      pipeline_create_info->GetPipelineLayout();

  const auto& context_vk = ContextVK::Cast(context);
  const auto& pool = context_vk.GetDescriptorPool();

  vk::DescriptorSetAllocateInfo alloc_info;
  std::array<vk::DescriptorSetLayout, 1> dsls = {
      pipeline_create_info->GetDescriptorSetLayout(),
  };

  alloc_info.setDescriptorPool(pool->GetPool());
  alloc_info.setSetLayouts(dsls);

  auto desc_sets_res = device_.allocateDescriptorSets(alloc_info);
  if (desc_sets_res.result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Failed to allocate descriptor sets: "
                   << vk::to_string(desc_sets_res.result);
    return false;
  }

  auto desc_sets = desc_sets_res.value;
  bool update_vertex_descriptors = UpdateDescriptorSets(
      "vertex_bindings", command.vertex_bindings, allocator, desc_sets[0]);
  if (!update_vertex_descriptors) {
    return false;
  }
  bool update_frag_descriptors = UpdateDescriptorSets(
      "fragment_bindings", command.fragment_bindings, allocator, desc_sets[0]);
  if (!update_frag_descriptors) {
    return false;
  }

  command_buffer_.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                     pipeline_layout, 0, desc_sets, nullptr);
  return true;
}

bool RenderPassVK::UpdateDescriptorSets(const char* label,
                                        const Bindings& bindings,
                                        Allocator& allocator,
                                        vk::DescriptorSet desc_set) const {
  std::vector<vk::WriteDescriptorSet> writes;
  std::vector<vk::DescriptorBufferInfo> buffer_infos;
  for (const auto& [buffer_index, view] : bindings.buffers) {
    const auto& buffer_view = view.resource.buffer;

    auto device_buffer = buffer_view->GetDeviceBuffer(allocator);
    if (!device_buffer) {
      VALIDATION_LOG << "Failed to get device buffer for vertex binding";
      return false;
    }

    auto buffer = DeviceBufferVK::Cast(*device_buffer).GetVKBufferHandle();
    if (!buffer) {
      return false;
    }

    // reserved index used for per-vertex data.
    if (buffer_index == VertexDescriptor::kReservedVertexBufferIndex) {
      continue;
    }

    uint32_t offset = view.resource.range.offset;

    vk::DescriptorBufferInfo desc_buffer_info;
    desc_buffer_info.setBuffer(buffer);
    desc_buffer_info.setOffset(offset);
    desc_buffer_info.setRange(view.resource.range.length);
    buffer_infos.push_back(desc_buffer_info);

    const ShaderUniformSlot& uniform = bindings.uniforms.at(buffer_index);

    vk::WriteDescriptorSet setWrite;
    setWrite.setDstSet(desc_set);
    setWrite.setDstBinding(uniform.binding);
    setWrite.setDescriptorCount(1);
    setWrite.setDescriptorType(vk::DescriptorType::eUniformBuffer);
    setWrite.setPBufferInfo(&buffer_infos.back());

    writes.push_back(setWrite);
  }

  std::array<vk::CopyDescriptorSet, 0> copies;
  device_.updateDescriptorSets(writes, copies);

  return true;
}

void RenderPassVK::SetViewportAndScissor(const Command& command) const {
  // set viewport.
  const auto& vp = command.viewport.value_or<Viewport>(
      {.rect = Rect::MakeSize(GetRenderTargetSize())});
  vk::Viewport viewport = vk::Viewport()
                              .setWidth(vp.rect.size.width)
                              .setHeight(vp.rect.size.height)
                              .setMinDepth(0.0f)
                              .setMaxDepth(1.0f);
  command_buffer_.setViewport(0, 1, &viewport);

  // scissor
  const auto& sc =
      command.scissor.value_or(IRect::MakeSize(GetRenderTargetSize()));
  vk::Rect2D scissor =
      vk::Rect2D()
          .setOffset(vk::Offset2D(sc.origin.x, sc.origin.y))
          .setExtent(vk::Extent2D(sc.size.width, sc.size.height));
  command_buffer_.setScissor(0, 1, &scissor);
}

vk::UniqueFramebuffer RenderPassVK::CreateFrameBuffer(
    const WrappedTextureInfoVK& wrapped_texture_info) const {
  auto img_view = wrapped_texture_info.swapchain_image->GetImageView();
  auto size = wrapped_texture_info.swapchain_image->GetSize();
  vk::FramebufferCreateInfo fb_create_info = vk::FramebufferCreateInfo()
                                                 .setRenderPass(render_pass_)
                                                 .setAttachmentCount(1)
                                                 .setPAttachments(&img_view)
                                                 .setWidth(size.width)
                                                 .setHeight(size.height)
                                                 .setLayers(1);
  auto res = device_.createFramebufferUnique(fb_create_info);
  FML_CHECK(res.result == vk::Result::eSuccess);
  return std::move(res.value);
}

}  // namespace impeller