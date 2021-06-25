#include <cstddef>
#include <fbxsdk.h>
#include <iostream>
#include <liblava/lava.hpp>
#include <typeinfo>

#define success(x, str)                                                        \
  if (!x) {                                                                    \
    std::cout << str << std::endl;                                             \
  }

#define fn auto

using fbxsdk::FbxNode;

enum RenderMode { mesh, skeleton };
RenderMode render_mode = mesh;

fn read_uv(FbxMesh *mesh, int texture_uv_index)->lava::v2 {
  auto uv = lava::v2();
  FbxGeometryElementUV *vertex_uv = mesh->GetElementUV();
  uv.x = (float)vertex_uv->GetDirectArray().GetAt(texture_uv_index)[0];
  uv.y = (float)vertex_uv->GetDirectArray().GetAt(texture_uv_index)[1];
  return uv;
}

fn read_mesh(FbxNode *node)->lava::mesh_data {
  lava::mesh_data output;
  FbxMesh *mesh = node->GetMesh();
  FbxSkin *skin = (FbxSkin *)mesh->GetDeformer(0, FbxDeformer::eSkin);
  size_t tri_count = mesh->GetPolygonCount();
  FbxVector4 *ctrl_points = mesh->GetControlPoints();
  for (size_t i = 0; i < tri_count; i++) {
    for (size_t j = 0; j < 3; j++) {
      size_t ctrl_index = mesh->GetPolygonVertex(i, j);
      output.vertices.push_back(
          lava::vertex{.position =
                           lava::v3{
                               static_cast<float>(ctrl_points[ctrl_index][0]),
                               static_cast<float>(ctrl_points[ctrl_index][1]),
                               static_cast<float>(ctrl_points[ctrl_index][2]),
                           },
                       .color = lava::v4{1, 1, 1, 1},
                       .uv = read_uv(mesh, mesh->GetTextureUVIndex(i, j)),
                       .normal = lava::v3{
                           static_cast<float>(
                               mesh->GetElementNormal()->GetDirectArray().GetAt(
                                   ctrl_index)[0]),
                           static_cast<float>(
                               mesh->GetElementNormal()->GetDirectArray().GetAt(
                                   ctrl_index)[1]),
                           static_cast<float>(
                               mesh->GetElementNormal()->GetDirectArray().GetAt(
                                   ctrl_index)[2]),
                       }});
      // Mirror UVs.
      output.vertices[output.vertices.size() - 1].uv =
          lava::v2{output.vertices[output.vertices.size() - 1].uv.x,
                   -output.vertices[output.vertices.size() - 1].uv.y};
    }
  }
  return output;
}

fn find_fbx_mesh(FbxNode *node)->std::optional<lava::mesh_data> {
  FbxNodeAttribute *attribute = node->GetNodeAttribute();
  if (attribute != nullptr) {
    if (attribute->GetAttributeType() == FbxNodeAttribute::eMesh) {
      return read_mesh(node);
    }
  }
  for (size_t i = 0; i < node->GetChildCount(); i++) {
    auto maybe_mesh = find_fbx_mesh(node->GetChild(i));
    if (maybe_mesh.has_value()) {
      return maybe_mesh;
    }
  }
  return std::nullopt;
}

typedef struct {
  // std::unique_ptr<FbxNode> node;
  FbxNode *node;
  int parent_index;
  FbxAMatrix transform;
  // glm::mat4x4 transform;
} Joint;

std::vector<Joint> joints;

// typedef struct {
// } Pose;

typedef struct {
  double time;
  std::vector<glm::mat4x4> joints;
} Keyframe;
typedef struct {
  double duration;
  std::vector<Keyframe> frames;
} AnimationClip;

void find_fbx_poses(FbxNode *node, std::vector<FbxPose *> *poses)
// ->std::optional<FbxPose *>
{
  if (node->GetNodeAttribute()) {
    if (node->GetNodeAttribute()->GetAttributeType()) {
      if (node->GetNodeAttribute()->GetAttributeType() ==
          FbxNodeAttribute::eSkeleton) {
        poses->push_back((FbxPose *)node);
      }
    }
  }
  for (size_t i = 0; i < node->GetChildCount(); i++) {
    FbxNode *child = node->GetChild(i);
    find_fbx_poses(child, poses);
  }
}

fn make_mesh_pipeline(lava::app &app, lava::graphics_pipeline::ptr &pipeline,
                      lava::descriptor::ptr &descriptor_layout,
                      lava::descriptor::pool::ptr &descriptor_pool,
                      lava::pipeline_layout::ptr &pipeline_layout,
                      VkDescriptorSet &descriptor_set,
                      lava::buffer &model_buffer,
                      lava::texture::ptr &loaded_texture) {
  pipeline = make_graphics_pipeline(app.device);
  success((pipeline->add_shader(lava::file_data("../res/vert.spv"),
                                VK_SHADER_STAGE_VERTEX_BIT)),
          "Failed to load vertex shader.");
  success((pipeline->add_shader(lava::file_data("../res/frag.spv"),
                                VK_SHADER_STAGE_FRAGMENT_BIT)),
          "Failed to load fragment shader.");
  pipeline->add_color_blend_attachment();
  pipeline->set_depth_test_and_write();
  pipeline->set_depth_compare_op(VK_COMPARE_OP_LESS_OR_EQUAL);
  pipeline->set_vertex_input_binding(
      {0, sizeof(lava::vertex), VK_VERTEX_INPUT_RATE_VERTEX});
  pipeline->set_vertex_input_attributes({
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
       lava::to_ui32(offsetof(lava::vertex, position))},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
       lava::to_ui32(offsetof(lava::vertex, color))},
      {2, 0, VK_FORMAT_R32G32_SFLOAT,
       lava::to_ui32(offsetof(lava::vertex, uv))},
      {3, 0, VK_FORMAT_R32G32B32_SFLOAT,
       lava::to_ui32(offsetof(lava::vertex, normal))},
  });
  descriptor_layout = lava::make_descriptor();
  descriptor_layout->add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                 VK_SHADER_STAGE_VERTEX_BIT);
  descriptor_layout->add_binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                 VK_SHADER_STAGE_VERTEX_BIT);
  descriptor_layout->add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 VK_SHADER_STAGE_FRAGMENT_BIT);
  success((descriptor_layout->create(app.device)),
          "Failed to create descriptor layout.");
  descriptor_pool = lava::make_descriptor_pool();
  success((descriptor_pool->create(
              app.device,
              {
                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
              })),
          "Failed to create descriptor pool.");
  pipeline_layout = lava::make_pipeline_layout();
  pipeline_layout->add(descriptor_layout);
  success((pipeline_layout->create(app.device)),
          "Failed to create pipeline layout.");
  pipeline->set_layout(pipeline_layout);
  descriptor_set = descriptor_layout->allocate(descriptor_pool->get());
  VkWriteDescriptorSet const write_desc_ubo_camera{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo = app.camera.get_descriptor_info(),
  };
  VkWriteDescriptorSet const write_desc_ubo_model{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = 1,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo = model_buffer.get_descriptor_info(),
  };
  VkWriteDescriptorSet const write_desc_sampler{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = 2,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = loaded_texture->get_descriptor_info(),
  };

  app.device->vkUpdateDescriptorSets(
      {write_desc_ubo_camera, write_desc_ubo_model, write_desc_sampler});

  lava::render_pass::ptr render_pass = app.shading.get_pass();
  success((pipeline->create(render_pass->get())), "Failed to make pipeline.");
  render_pass->add_front(pipeline);
}

int main(int argc, char *argv[]) {
  // Load and read the mesh from an FBX.
  // std::string path = "../res/Teddy/Teddy_Idle.fbx";
  std::string path = "../res/Idle.fbx";
  FbxManager *manager = FbxManager::Create();
  FbxIOSettings *io_settings = FbxIOSettings::Create(manager, IOSROOT);
  manager->SetIOSettings(io_settings);
  FbxImporter *importer = FbxImporter::Create(manager, "");
  FbxScene *scene = FbxScene::Create(manager, "");
  success(importer->Initialize(path.c_str(), -1, manager->GetIOSettings()),
          "Failed to import");
  importer->Import(scene);
  importer->Destroy();
  FbxNode *root_node = scene->GetRootNode();
  lava::mesh_data loaded_data = find_fbx_mesh(root_node).value();
  // manager->Destroy();
  std::cout << "Path: " << path << std::endl;

  // Load the skeleton.
  // Keyframe keyframe;
  std::vector<FbxPose *> poses;
  // Fill out poses.
  find_fbx_poses(root_node, &poses);
  FbxPose *bind_pose = nullptr;
  std::cout << "Poses: " << poses.size() << std::endl;
  for (size_t i = 0; i < poses.size(); i++) {
    if (poses[i]->IsBindPose()) {
      std::cout << "Found a bind pose.\n";
      bind_pose = poses[i];
      break;
    }
  }
  auto pose_count = scene->GetPoseCount();
  for (size_t i = 0; i < pose_count; i++) {
    auto cur_pose = scene->GetPose(i);
    if (cur_pose->IsBindPose()) {
      bind_pose = cur_pose;
      break;
    }
  }
  success(bind_pose, "Failed to find a bind pose.\n");

  // Find skeleton.
  FbxSkeleton *root_skel = nullptr;
  for (size_t i = 0; i < bind_pose->GetCount(); i++) {
    auto cur_skel = bind_pose->GetNode(i)->GetSkeleton();
    if (cur_skel && cur_skel->IsSkeletonRoot()) {
      root_skel = cur_skel;
    }
  }
  success(root_skel, "Failed to find a root skeleton.");

  std::vector<Joint *> joints;
  auto make_joint = [&](FbxNode *node, int index) -> Joint * {
    // TODO: Factor into unique ptr.
    return new Joint{.node = node,
                     .parent_index = index,
                     .transform = node->EvaluateGlobalTransform()};
  };
  std::function<void(Joint *, int)> get_joints = [&](Joint *joint, int index) {
    for (size_t i = 0; i < joint->node->GetChildCount(); i++) {
      auto cur_node = joint->node->GetChild(i);
      if (cur_node && cur_node->GetNodeAttribute() &&
          cur_node->GetNodeAttribute()->GetAttributeType() ==
              FbxNodeAttribute::eSkeleton) {
        auto new_joint =
            make_joint(cur_node, index + joint->node->GetChildCount() + i);
        joints.push_back(new_joint);
        get_joints(new_joint, index + i);
      }
    }
  };
  get_joints(make_joint(root_skel->GetNode(), -1), -1);

  // Render the mesh.
  lava::app app("DEV 5 - WGooch", {argc, argv});
  app.config.surface.formats = {VK_FORMAT_B8G8R8A8_SRGB};
  success(app.setup(), "Failed to setup app.");
  lava::mesh::ptr made_mesh = lava::make_mesh();
  made_mesh->add_data(loaded_data);
  lava::texture::ptr loaded_texture =
      // create_default_texture(app.device, {4096, 4096});
      lava::load_texture(app.device, "../res/Idle.fbm/PPG_3D_Player_D.png");
  app.staging.add(loaded_texture);
  app.camera.position = lava::v3(0.0f, -4.036f, 8.304f);
  app.camera.rotation = lava::v3(-15, 0, 0);
  lava::mat4 model_space = lava::mat4(1.0); // This is an identity matrix.
  // lava::mat4 model_space = glm::identity<lava::mat4>();
  lava::buffer model_buffer;
  success(model_buffer.create_mapped(app.device, &model_space,
                                     sizeof(float) * 16,
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
          "Failed to map buffer.");
  made_mesh->create(app.device);
  lava::graphics_pipeline::ptr mesh_pipeline;
  lava::descriptor::pool::ptr mesh_descriptor_pool;
  lava::descriptor::ptr mesh_descriptor_layout;
  lava::pipeline_layout::ptr mesh_pipeline_layout;
  VkDescriptorSet mesh_descriptor_set = VK_NULL_HANDLE;
  app.on_create = [&]() {
    make_mesh_pipeline(app, mesh_pipeline, mesh_descriptor_layout,
                       mesh_descriptor_pool, mesh_pipeline_layout,
                       mesh_descriptor_set, model_buffer, loaded_texture);
    return true;
  };

  app.on_update = [&](lava::delta dt) {
    // Command buffers
    if (render_mode == mesh) {
      mesh_pipeline->on_process = [&](VkCommandBuffer cmd_buf) {
        mesh_pipeline_layout->bind(cmd_buf, mesh_descriptor_set);
        made_mesh->bind_draw(cmd_buf);
      };
    } else if (render_mode == skeleton) {
      mesh_pipeline->on_process = [&](VkCommandBuffer cmd_buf) {
        // pipeline_layout->bind(cmd_buf, descriptor_set);
        // made_mesh->bind_draw(cmd_buf);
      };
    }
    app.camera.update_view(dt, app.input.get_mouse_position());
    return true;
  };

  app.input.key.listeners.add([&](lava::key_event::ref event) {
    if (event.pressed(lava::key::_1)) {
      std::cout << "Rendering the mesh." << std::endl;
      render_mode = mesh;
      return true;
    } else if (event.pressed(lava::key::_2)) {
      std::cout << "Rendering the skeleton." << std::endl;
      render_mode = skeleton;
      return true;
    }
    return false;
  });

  return app.run();
}
