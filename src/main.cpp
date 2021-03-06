#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL

#include <fbxsdk.h>
#include <imgui.h>

#include <cstddef>
#include <glm/gtx/string_cast.hpp>
#include <iostream>
#include <liblava/lava.hpp>
#include <typeinfo>

#include "fbx_loading.h"
#include "includes.h"
#include "pipelines.h"

using fbxsdk::FbxNode;

enum RenderMode { mesh, skeleton };
static RenderMode render_mode;  // Initialized in main()
// static std::vector<AnimationClip> anim_clips;
static size_t current_keyframe_index = 0;
static double current_keyframe_time;
static bool animating = true;

int main(int argc, char *argv[]) {
  // Load and read the mesh from an FBX.
  // std::string path = "../res/Teddy/Teddy_Idle.fbx";
  std::string path = "../res/Idle.fbx";
  FbxManager *fbx_manager = FbxManager::Create();
  FbxIOSettings *io_settings = FbxIOSettings::Create(fbx_manager, IOSROOT);
  fbx_manager->SetIOSettings(io_settings);
  FbxImporter *importer = FbxImporter::Create(fbx_manager, "");
  FbxScene *scene = FbxScene::Create(fbx_manager, "");
  success(importer->Initialize(path.c_str(), -1, fbx_manager->GetIOSettings()),
          "Failed to import");
  importer->Import(scene);
  importer->Destroy();
  FbxNode *root_node = scene->GetRootNode();
  lava::mesh_template_data<skin_vertex> loaded_data =
      find_fbx_mesh(root_node).value();
  FbxSkin *skin = find_fbx_skin(root_node);

  // Load the skeleton.
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

  std::vector<Joint> joints;
  auto make_joint = [&](FbxNode *node, int index) -> Joint {
    return Joint{.node = node,
                 .parent_index = index,
                 .transform = node->EvaluateGlobalTransform()};
  };
  std::function<void(Joint, int, size_t)> get_joints =
      [&](Joint parent_joint, int parent_index, size_t parent_breadth) {
        size_t children = parent_joint.node->GetChildCount();
        for (size_t i = 0; i < children; i++) {
          auto cur_node = parent_joint.node->GetChild(i);
          if (cur_node && cur_node->GetNodeAttribute() &&
              cur_node->GetNodeAttribute()->GetAttributeType() ==
                  FbxNodeAttribute::eSkeleton) {
            auto new_joint = make_joint(cur_node, parent_index);
            joints.push_back(new_joint);
            get_joints(new_joint, joints.size() - 1, children);
          }
        }
      };

  get_joints(make_joint(root_skel->GetNode(), -1), 0, 0);
  std::cout << "SKEL NODES: " << root_skel->GetNodeCount() << '\n';
  std::cout << "JOINTS SIZE: " << joints.size() << '\n';

  // Render the mesh.
  lava::app app("DEV 5 - WGooch", {argc, argv});
  app.config.surface.formats = {VK_FORMAT_B8G8R8A8_SRGB};
  app.camera.rotation_speed = 250;
  app.camera.movement_speed += 10;
  success(app.setup(), "Failed to setup app.");

  app.camera.position = lava::v3(0.0f, -4.036f, 8.304f);
  app.camera.rotation = lava::v3(-15, 0, 0);
  app.camera.set_movement_keys({lava::key::m},
                               {lava::key::n, lava::key::left_control},
                               {lava::key::s, lava::key::left_super},
                               {lava::key::t, lava::key::left_alt});
  lava::mat4 mesh_model_mat = lava::mat4(1.0);  // This is an identity matrix.

  // Bones
  lava::buffer bones_buffer;
  lava::mesh_data bone_mesh_data;
  // TODO: Reserve size of vectors.
  std::vector<lava::mat4> bones_inverse_bind_mats;
  std::vector<Transform> bones_keyframe_cur_global_transforms;
  std::vector<Transform> bones_keyframe_next_global_transforms;
  std::vector<float> bones_weights;

  for (size_t i = 0; i < joints.size(); i++) {
    Joint current_joint = joints[i];
    lava::mat4 current_matrix = glm::identity<glm::dmat4x4>();
    // FBX Matrices are column-major double-precision floating-point.
    current_matrix = fbxmat_to_lavamat(joints[i].transform);
    glm::quat current_quaternion = glm::quat_cast(current_matrix);
    lava::v3 current_translation = current_matrix[3];

    bone_mesh_data.vertices.push_back(lava::vertex{
        .position = lava::v3(0, 0, 0),
        .color = lava::v4(1, 1, 1, 1),
    });
    bone_mesh_data.indices.push_back(i);
    bone_mesh_data.indices.push_back(current_joint.parent_index);

    bones_inverse_bind_mats.push_back(glm::inverse(current_matrix));

    // Push bind pose by default.
    bones_keyframe_cur_global_transforms.push_back(
        Transform{current_translation, current_quaternion});
    bones_keyframe_next_global_transforms.push_back(
        Transform{current_translation, current_quaternion});
  }

  lava::mesh::ptr bones_mesh = lava::make_mesh();
  bones_mesh->add_data(bone_mesh_data);
  bones_mesh->create(app.device);

  // Skinning clusters
  struct skin_control_point {
    std::array<int, 4> joint_indices;
    std::array<float, 4> joint_weights{0, 0, 0, 0};
    fn normalize() {
      float weight_sum = 0;
      for (auto &weight : joint_weights) {
        weight_sum += weight;
      }
      for (auto &weight : joint_weights) {
        weight /= weight_sum;
      }
    }
  };
  std::vector<skin_control_point> skin_control_points;

  for (size_t i = 0; i < skin->GetClusterCount(); i++) {
    FbxCluster *current_cluster = skin->GetCluster(i);
    FbxNode *current_node = current_cluster->GetLink();
    double *weights = current_cluster->GetControlPointWeights();
    int *control_points = current_cluster->GetControlPointIndices();
    int joint_index;
    for (size_t i = 0; i < joints.size(); i++) {
      if (joints[i].node == current_node) {
        joint_index = i;
        break;
      }
    }
    skin_control_point current_control_point;
    for (size_t i = 0; i < current_cluster->GetControlPointIndicesCount();
         i++) {
      if (i > 4) {
        // TODO: Grab the four greatest weights, not the first four.
        break;
      }
      current_control_point.joint_weights[i] = weights[i];
      current_control_point.joint_indices[i] = control_points[i];
    }
    current_control_point.normalize();
    skin_control_points.push_back(current_control_point);
  }

  // Put skin data into mesh data.
  for (size_t i = 0; i < loaded_data.vertices.size(); i++) {
    auto current_vertex = &loaded_data.vertices[i];
    for (size_t j = 0; j < skin_control_points.size(); j++) {
      auto indices = skin_control_points[j].joint_indices;
      if (std::find(std::begin(indices), std::end(indices), i)) {
      }
    }
    // current_vertex->weight_indices;
  }

  // Load animation.
  auto fps = FbxTime::EMode::eFrames24;
  FbxAnimStack *anim_stack = scene->GetCurrentAnimationStack();
  FbxTimeSpan time_span = anim_stack->GetLocalTimeSpan();
  FbxTime real_time = time_span.GetDuration();
  AnimationClip anim_clip{};
  anim_clip.duration = real_time.GetFrameCount(fps);

  // Make an array of joint transforms for every keyframe in the animation
  // clip. Start at 1, because 0 is the bind pose frame.
  for (double i = 1; i < anim_clip.duration; i++) {
    Keyframe current_keyframe;
    real_time.SetFrame(i, fps);
    current_keyframe.time = i;
    current_keyframe.transforms.reserve(joints.size());
    for (auto joint : joints) {
      lava::mat4 current_matrix =
          fbxmat_to_lavamat(joint.node->EvaluateGlobalTransform(real_time));
      glm::quat current_quaternion = glm::quat_cast(current_matrix);
      lava::v3 current_translation = current_matrix[3];
      current_keyframe.transforms.push_back(
          Transform{current_translation, current_quaternion});
    }
    anim_clip.frames.push_back(current_keyframe);
  }

  // Load textures
  // TODO: Abstract as function
  lava::texture::ptr diffuse_texture =
      lava::load_texture(app.device, "../res/Idle.fbm/PPG_3D_Player_D.png");
  lava::texture::ptr emissive_texture = lava::load_texture(
      app.device, "../res/Idle.fbm/PPG_3D_Player_emissive.png");
  lava::texture::ptr normal_texture =
      lava::load_texture(app.device, "../res/Idle.fbm/PPG_3D_Player_N.png");
  lava::texture::ptr specular_texture =
      lava::load_texture(app.device, "../res/Idle.fbm/PPG_3D_Player_spec.png");
  app.staging.add(diffuse_texture);
  app.staging.add(emissive_texture);
  app.staging.add(normal_texture);
  app.staging.add(specular_texture);
  std::array<VkDescriptorImageInfo, 4> textures_descriptor_info{{
      *diffuse_texture->get_descriptor_info(),
      *emissive_texture->get_descriptor_info(),
      *normal_texture->get_descriptor_info(),
      *specular_texture->get_descriptor_info(),
  }};

  typedef struct {
    lava::mat4 view_proj;
    alignas(16) lava::v3 cam_pos;
  } CameraBuffer;

  CameraBuffer camera_buffer_data = {lava::mat4(1), app.camera.position};

  lava::buffer camera_buffer;
  camera_buffer.create_mapped(
      app.device, &camera_buffer_data,
      sizeof(lava::mat4) + sizeof(app.camera.position),
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

  // Load mesh.
  auto made_mesh = lava::make_mesh<skin_vertex>();
  made_mesh->add_data(loaded_data);
  made_mesh->create(app.device);
  lava::buffer object_buffer;
  object_buffer.create_mapped(app.device, &mesh_model_mat,
                              sizeof(mesh_model_mat),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

  // Make bone buffers
  lava::buffer bone_object_buffer;
  bone_object_buffer.create_mapped(app.device, &mesh_model_mat,
                                   sizeof(lava::mat4),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  lava::buffer bone_inverse_bind_mats_buffer;
  bone_inverse_bind_mats_buffer.create_mapped(
      app.device, &bones_inverse_bind_mats[0],
      bones_inverse_bind_mats.size() * sizeof(lava::mat4),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  lava::buffer keyframe_cur_trans_buffer;
  keyframe_cur_trans_buffer.create_mapped(
      app.device, &bones_keyframe_cur_global_transforms[0],
      bones_keyframe_cur_global_transforms.size() * sizeof(Transform),
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  lava::buffer keyframe_next_trans_buffer;
  keyframe_next_trans_buffer.create_mapped(
      app.device, &bones_keyframe_next_global_transforms[0],
      bones_keyframe_next_global_transforms.size() * sizeof(Transform),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  lava::buffer animation_keyframe_buffer;
  animation_keyframe_buffer.create_mapped(app.device, &current_keyframe_time,
                                          1 * sizeof(float),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  lava::graphics_pipeline::ptr mesh_pipeline;
  lava::descriptor::ptr mesh_descriptor_layout_global;
  lava::descriptor::ptr mesh_descriptor_layout_textures;
  lava::descriptor::ptr mesh_descriptor_layout_object;
  lava::pipeline_layout::ptr mesh_pipeline_layout;
  VkDescriptorSet mesh_descriptor_set_global = VK_NULL_HANDLE;
  VkDescriptorSet mesh_descriptor_set_textures = VK_NULL_HANDLE;
  VkDescriptorSet mesh_descriptor_set_object = VK_NULL_HANDLE;
  VkDescriptorSet mesh_descriptor_set_animation = VK_NULL_HANDLE;

  lava::graphics_pipeline::ptr bone_pipeline;
  lava::pipeline_layout::ptr bone_pipeline_layout;
  VkDescriptorSet bone_descriptor_set_global = VK_NULL_HANDLE;
  VkDescriptorSet bone_descriptor_set_object = VK_NULL_HANDLE;

  lava::descriptor::pool::ptr descriptor_pool;
  descriptor_pool = lava::make_descriptor_pool();
  descriptor_pool->create(app.device,
                          {
                              {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 50},
                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 50},
                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 40},
                          },
                          90);

  app.on_create = [&]() {
    // TODO: Push descriptors to this, then update all here.
    // std::vector<VkWriteDescriptorSet> descriptor_writes;
    auto [mesh_descriptor_layout_global, mesh_descriptor_layout_textures,
          mesh_descriptor_layout_object, mesh_descriptor_layout_animation] =
        create_mesh_descriptor_layout(app);
    mesh_pipeline_layout = lava::make_pipeline_layout();
    mesh_pipeline_layout->add(mesh_descriptor_layout_global);
    mesh_pipeline_layout->add(mesh_descriptor_layout_textures);
    mesh_pipeline_layout->add(mesh_descriptor_layout_object);
    mesh_pipeline_layout->add(mesh_descriptor_layout_animation);
    mesh_pipeline_layout->create(app.device);
    mesh_descriptor_set_global =
        mesh_descriptor_layout_global->allocate(descriptor_pool->get());
    mesh_descriptor_set_textures =
        mesh_descriptor_layout_textures->allocate(descriptor_pool->get());
    mesh_descriptor_set_object =
        mesh_descriptor_layout_object->allocate(descriptor_pool->get());
    mesh_descriptor_set_animation =
        mesh_descriptor_layout_animation->allocate(descriptor_pool->get());

    auto [bone_descriptor_layout_global, bone_descriptor_layout_object] =
        create_bone_descriptors_layout(app);
    bone_pipeline_layout = lava::make_pipeline_layout();
    bone_pipeline_layout->add(bone_descriptor_layout_global);
    bone_pipeline_layout->add(bone_descriptor_layout_object);
    bone_pipeline_layout->create(app.device);

    bone_descriptor_set_global =
        bone_descriptor_layout_global->allocate(descriptor_pool->get());
    bone_descriptor_set_object =
        bone_descriptor_layout_object->allocate(descriptor_pool->get());

    // TODO: Move descriptor writes into a new func that also does descriptor
    // layout allocation.
    {
      VkWriteDescriptorSet const descriptor_global{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = mesh_descriptor_set_global,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = camera_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_textures{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = mesh_descriptor_set_textures,
          .dstBinding = 0,
          .descriptorCount = 4,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &textures_descriptor_info.front(),
      };
      VkWriteDescriptorSet const descriptor_object{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = mesh_descriptor_set_object,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = object_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_animation_mesh_inversebind{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = mesh_descriptor_set_animation,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = bone_inverse_bind_mats_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_animation_mesh_keyframe_trans_curr{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = mesh_descriptor_set_animation,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = keyframe_cur_trans_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_animation_mesh_keyframe_trans_next{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = mesh_descriptor_set_animation,
          .dstBinding = 2,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = keyframe_next_trans_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_animation_mesh_keyframe_time{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = mesh_descriptor_set_animation,
          .dstBinding = 3,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = animation_keyframe_buffer.get_descriptor_info(),
      };

      VkWriteDescriptorSet const descriptor_global_bone{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = bone_descriptor_set_global,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = camera_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_object_bone_model{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = bone_descriptor_set_object,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = bone_object_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_object_bone_inversebind{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = bone_descriptor_set_object,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = bone_inverse_bind_mats_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_object_bone_keyframe_trans_curr{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = bone_descriptor_set_object,
          .dstBinding = 2,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = keyframe_cur_trans_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_object_bone_keyframe_trans_next{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = bone_descriptor_set_object,
          .dstBinding = 3,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = keyframe_next_trans_buffer.get_descriptor_info(),
      };
      VkWriteDescriptorSet const descriptor_object_bone_keyframe_time{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = bone_descriptor_set_object,
          .dstBinding = 4,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = animation_keyframe_buffer.get_descriptor_info(),
      };

      app.device->vkUpdateDescriptorSets({
          descriptor_global,
          descriptor_textures,
          descriptor_object,
          descriptor_animation_mesh_inversebind,
          descriptor_animation_mesh_keyframe_trans_curr,
          descriptor_animation_mesh_keyframe_trans_next,
          descriptor_animation_mesh_keyframe_time,
          descriptor_global_bone,
          descriptor_object_bone_model,
          descriptor_object_bone_inversebind,
          descriptor_object_bone_keyframe_trans_curr,
          descriptor_object_bone_keyframe_trans_next,
          descriptor_object_bone_keyframe_time,
      });

      // Loading shaders
      {
        using shader_module_t = std::tuple<std::string, VkShaderStageFlagBits>;
        auto shader_modules = std::vector<shader_module_t>();
        shader_modules.push_back(
            shader_module_t("../res/vert.spv", VK_SHADER_STAGE_VERTEX_BIT));
        shader_modules.push_back(
            shader_module_t("../res/frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT));
        mesh_pipeline = create_graphics_pipeline<skin_vertex>(
            app, mesh_pipeline_layout, shader_modules,
            {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                 (offsetof(skin_vertex, position))},
                {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                 (offsetof(skin_vertex, color))},
                {2, 0, VK_FORMAT_R32G32_SFLOAT, (offsetof(skin_vertex, uv))},
                {3, 0, VK_FORMAT_R32G32B32_SFLOAT,
                 (offsetof(skin_vertex, normal))},
                {4, 0, VK_FORMAT_R32G32B32A32_UINT,
                 (offsetof(skin_vertex, weight_indices))},
                {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                 (offsetof(skin_vertex, bone_weights))},
            },
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
      }
    }

    {
      // TODO: Better line shaders
      using shader_module_t = std::tuple<std::string, VkShaderStageFlagBits>;
      auto shader_modules = std::vector<shader_module_t>();
      shader_modules.push_back(
          shader_module_t("../res/line_vert.spv", VK_SHADER_STAGE_VERTEX_BIT));
      shader_modules.push_back(shader_module_t("../res/line_frag.spv",
                                               VK_SHADER_STAGE_FRAGMENT_BIT));
      bone_pipeline = create_graphics_pipeline<lava::vertex>(
          app, bone_pipeline_layout, shader_modules,
          {
              {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
               (offsetof(lava::vertex, position))},
              {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
               (offsetof(lava::vertex, color))},
          },
          VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    }

    // Default to rendering the mesh.
    render_mode = mesh;
    // render_mode = skeleton;
    return true;
  };

  app.imgui.on_draw = [&]() {
    ImGui::SetNextWindowPos({30, 30}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({330, 485}, ImGuiCond_FirstUseEver);
    ImGui::Begin(app.get_name());
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::DragFloat3("position##camera", (lava::r32 *)&app.camera.position,
                      0.01f);
    ImGui::DragFloat3("rotation##camera", (lava::r32 *)&app.camera.rotation,
                      0.1f);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("speed")) {
      ImGui::DragFloat("movement##camera", &app.camera.movement_speed, 0.1f);
      ImGui::DragFloat("rotation##camera", &app.camera.rotation_speed, 0.1f);
      ImGui::DragFloat("zoom##camera", &app.camera.zoom_speed, 0.1f);
    }
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Pause / Play")) animating = !animating;
    ImGui::Text("Keyframe %zu / %.3f", current_keyframe_index - 1,
                anim_clip.duration - 1.f);
    float keyframe_time_remainder = fmod(current_keyframe_time, 1.f);
    ImGui::SliderFloat("Between Frames", &keyframe_time_remainder, 0.f, 1.f);
    if (keyframe_time_remainder < 1.f) {
      current_keyframe_time = current_keyframe_index + keyframe_time_remainder;
    }
    if (ImGui::Button("Back Frame"))
      current_keyframe_time = floor(current_keyframe_time - 1);
    ImGui::SameLine();
    if (ImGui::Button("Next Frame"))
      current_keyframe_time = floor(current_keyframe_time + 1);
    ImGui::End();
  };

  app.input.key.listeners.add([&](lava::key_event::ref event) {
    if (event.pressed(lava::key::_1)) {
      render_mode = mesh;
      return true;
    } else if (event.pressed(lava::key::_2)) {
      render_mode = skeleton;
      return true;
    }
    return false;
  });

  app.on_update = [&](lava::delta dt) {
    app.camera.update_view(dt, app.input.get_mouse_position());
    app.camera.update_projection();
    mesh_pipeline->on_process = nullptr;
    bone_pipeline->on_process = nullptr;

    current_keyframe_time += dt * 10.f * animating;
    if (current_keyframe_time > anim_clip.duration) {
      current_keyframe_time = 1;
    } else if (current_keyframe_time == 0) {
      current_keyframe_time = anim_clip.duration;
    }
    current_keyframe_index = floor(current_keyframe_time);

    app.camera.update_view(dt, app.input.get_mouse_position());
    camera_buffer_data.view_proj = app.camera.get_view_projection();
    camera_buffer_data.cam_pos = app.camera.position;

    memcpy(camera_buffer.get_mapped_data(), &camera_buffer_data,
           // sizeof(camera_buffer_data));
           sizeof(lava::mat4) + sizeof(app.camera.position));

    if (render_mode == mesh) {
      memcpy(object_buffer.get_mapped_data(), &mesh_model_mat,
             sizeof(mesh_model_mat));

      mesh_pipeline->on_process = [&](VkCommandBuffer cmd_buf) {
        mesh_pipeline_layout->bind(cmd_buf, mesh_descriptor_set_global);
        mesh_pipeline_layout->bind(cmd_buf, mesh_descriptor_set_textures, 1);
        mesh_pipeline_layout->bind(cmd_buf, mesh_descriptor_set_object, 2);
        mesh_pipeline_layout->bind(cmd_buf, mesh_descriptor_set_animation, 3);
        made_mesh->bind_draw(cmd_buf);
      };
    } else if (render_mode == skeleton) {
      memcpy(keyframe_cur_trans_buffer.get_mapped_data(),
             &anim_clip.frames[current_keyframe_index].transforms[0],
             sizeof(anim_clip.frames[0].transforms[0]) *
                 anim_clip.frames[current_keyframe_index].transforms.size());
      memcpy(
          keyframe_next_trans_buffer.get_mapped_data(),
          &anim_clip.frames[current_keyframe_index + 1].transforms[0],
          sizeof(anim_clip.frames[0].transforms[0]) *
              anim_clip.frames[current_keyframe_index + 1].transforms.size());

      float keyframe_time_remainder = fmod(current_keyframe_time, 1.f);
      memcpy(animation_keyframe_buffer.get_mapped_data(),
             &keyframe_time_remainder, sizeof(float));

      bone_pipeline->on_process = [&](VkCommandBuffer cmd_buf) {
        bone_pipeline_layout->bind(cmd_buf, bone_descriptor_set_global);
        // TODO: Figure out how to make this work with binding at 2 instead
        // of 1:
        bone_pipeline_layout->bind(cmd_buf, bone_descriptor_set_object, 1);
        bones_mesh->bind_draw(cmd_buf);
      };
    }
    return true;
  };

  fbx_manager->Destroy();
  return app.run();
}
