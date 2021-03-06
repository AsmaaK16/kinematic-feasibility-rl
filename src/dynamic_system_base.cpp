#include <modulation_rl/dynamic_system_base.h>

namespace conf {
    double min_planner_velocity = 0.001;
    double max_planner_velocity = 0.1;
}

DynamicSystem_base::DynamicSystem_base(uint32_t seed,
                                       double min_goal_dist,
                                       double max_goal_dist,
                                       std::string strategy,
                                       std::string real_execution,
                                       bool init_controllers,
                                       double penalty_scaling,
                                       double time_step,
                                       double slow_down_real_exec,
                                       bool perform_collision_check,
                                       RoboConf robo_config) :
    ROSCommonNode(0, NULL, "ds"),
    nh_{new ros::NodeHandle("modulation_rl_ik")},
    rate_{50},
    rng_{seed},
    robo_config_{robo_config},
    strategy_{strategy},
    min_goal_dist_{min_goal_dist},
    max_goal_dist_{max_goal_dist},
    init_controllers_{init_controllers},
    penalty_scaling_{penalty_scaling},
    // success_thres_dist_ { 0.02 },
    // success_thres_rot_ { 0.05 },
    time_step_real_exec_{time_step},
    time_step_train_{0.1},
    perform_collision_check_{perform_collision_check} {
    if (perform_collision_check_ && (robo_config_.name == "hsr")) {
        throw std::runtime_error("find_ik() not adapted for HSR yet");
    }

    traj_visualizer_ = nh_->advertise<moveit_msgs::DisplayTrajectory>("traj_visualizer", 1);
    gripper_visualizer_ = nh_->advertise<visualization_msgs::Marker>("gripper_goal_visualizer", 1);
    robstate_visualizer_ = nh_->advertise<moveit_msgs::DisplayRobotState>("robot_state_visualizer", 50);
    ellipses_pub_ = nh_->advertise<visualization_msgs::MarkerArray>("/GMM/Ellipses", 1, true);
    cmd_base_vel_pub_ = nh_->advertise<geometry_msgs::Twist>(robo_config_.base_cmd_topic, 1);
    client_get_scene_ = nh_->serviceClient<moveit_msgs::GetPlanningScene>("/get_planning_scene");

    // https://readthedocs.org/projects/moveit/downloads/pdf/latest/
    // https://ros-planning.github.io/moveit_tutorials/doc/planning_scene_monitor/planning_scene_monitor_tutorial.html
    // ros::spinOnce();
    spinner_ = new ros::AsyncSpinner(2);
    spinner_->start();

    // Load Robot config from moveit movegroup (must be running)
    robot_model_loader::RobotModelLoaderPtr robot_model_loader;
    robot_model_loader.reset(new robot_model_loader::RobotModelLoader("robot_description"));

    robot_model::RobotModelPtr kinematic_model = robot_model_loader->getModel();
    kinematic_state_.reset(new robot_state::RobotState(kinematic_model));
    kinematic_state_->setToDefaultValues();
    joint_model_group_ = kinematic_model->getJointModelGroup(robo_config_.joint_model_group_name);

    // Set startstate for trajectory visualization
    joint_names_ = joint_model_group_->getVariableNames();
    link_names_ = joint_model_group_->getLinkModelNames();

    planning_scene_.reset(new planning_scene::PlanningScene(kinematic_model));
    ROS_INFO("Planning frame: %s", planning_scene_->getPlanningFrame().c_str());

    moveit_msgs::GetPlanningScene scene_srv1;
    moveit_msgs::PlanningScene currentScene;
    scene_srv1.request.components.components = 2;  // moveit_msgs::PlanningSceneComponents::ROBOT_STATE;
    if (!client_get_scene_.call(scene_srv1)) {
        ROS_WARN("Failed to call service /get_planning_scene");
    }
    planning_scene_->setPlanningSceneDiffMsg(scene_srv1.response.scene);
    robot_state::RobotState robstate = planning_scene_->getCurrentState();
    display_trajectory_.model_id = robo_config_.name;
    moveit_msgs::RobotState start_state;

    const std::vector<std::string> &all_joint_names = kinematic_model->getJointModelNames();
    for (int j = 0; j < all_joint_names.size(); j++) {
        const std::string name = all_joint_names[j];
        const double default_value = kinematic_state_->getJointPositions(name)[0];
        const double actual_value = robstate.getJointPositions(name)[0];

        // avoid adding joints that are not defined in other places (e.g. rviz)
        if (std::abs(default_value - actual_value) > 0.0000001) {
            // std::cout << name << ", " << default_value << ", " << actual_value << std::endl;
            start_state.joint_state.name.push_back(name);
            start_state.joint_state.position.push_back(actual_value);
            start_state.joint_state.velocity.push_back(0.0);
            // also update the values in the kinematic_state which are simply set to default above
            kinematic_state_->setJointPositions(name, &actual_value);
        }
    }
    start_state.multi_dof_joint_state.header.frame_id = robo_config_.frame_id;
    start_state.multi_dof_joint_state.joint_names.push_back("world_joint");
    geometry_msgs::Transform startTransform;
    startTransform.translation.x = 0;
    startTransform.translation.y = 0;
    startTransform.translation.z = 0;
    startTransform.rotation.x = 0;
    startTransform.rotation.y = 0;
    startTransform.rotation.z = 0;
    startTransform.rotation.w = 1;
    start_state.multi_dof_joint_state.transforms.push_back(startTransform);
    display_trajectory_.trajectory_start = start_state;

    set_real_execution(real_execution, time_step_real_exec_, slow_down_real_exec);

    // always do this so we can later change to real_execution
    if (init_controllers_) {
        planning_scene_monitor_.reset(new planning_scene_monitor::PlanningSceneMonitor(robot_model_loader));
        planning_scene_monitor_->startSceneMonitor("/my_planning_scene");
    }

    if (perform_collision_check_) {
        // Collision constraint function GroupStateValidityCallbackFn(),
        moveit_msgs::GetPlanningScene scene_srv;
        moveit_msgs::PlanningScene currentScene;
        scene_srv.request.components.components = 24;  // moveit_msgs::PlanningSceneComponents::WORLD_OBJECT_NAMES;
        if (!client_get_scene_.call(scene_srv)) {
            ROS_WARN("Failed to call service /get_planning_scene");
        }
        currentScene = scene_srv.response.scene;
        ROS_INFO("Known collision objects:");
        for (int i = 0; i < (int)scene_srv.response.scene.world.collision_objects.size(); ++i) {
            ROS_INFO_STREAM(scene_srv.response.scene.world.collision_objects[i].id);
        }
        planning_scene_->setPlanningSceneDiffMsg(currentScene);
        constraint_callback_fn_ = boost::bind(&validityFun::validityCallbackFn, planning_scene_, kinematic_state_, _2, _3);
    }

    if (strategy_ == "modulate_ellipse") {
        modulation_.setEllipses();
    }
}

void DynamicSystem_base::set_real_execution(std::string real_execution, double time_step, double slow_down_real_exec) {
    if (real_execution == "gazebo") {
        world_ = new GazeboWorld();
    } else if (real_execution == "world") {
        world_ = new RealWorld();
    } else if (real_execution == "sim") {
        world_ = new SimWorld();
    } else {
        throw std::runtime_error("Unknown real_execution value");
    }
    if ((!world_->is_analytical()) && !init_controllers_) {
        throw std::runtime_error("must have initialised controllers to use real_execution_");
    }
    time_step_real_exec_ = time_step;
    slow_down_factor_ = (world_->is_analytical()) ? 1.0 : slow_down_real_exec;
}

void DynamicSystem_base::set_new_random_goal(std::string gripper_goal_distribution) {
    // slightly hacky / hardcoded real world case to ensure we get a random goal in a valid part of the map
    tf::Transform currentBase;
    double min_goal_height = (gripper_goal_distribution == "restricted_ws") ? robo_config_.restricted_ws_z_min : robo_config_.z_min;
    double max_goal_height = (gripper_goal_distribution == "restricted_ws") ? robo_config_.restricted_ws_z_max : robo_config_.z_max;

    // only used in real world execution
    if (world_->get_name() == "world") {
        currentBase = world_->get_base_transform_world();
    } else {
        currentBase.setIdentity();
    }
    bool valid = false;
    while (!valid) {
        // random goal around the origin
        currentGripperGOAL_.setIdentity();
        double goal_dist = rng_.uniformReal(min_goal_dist_, max_goal_dist_);
        double goal_orientation = rng_.uniformReal(0.0, M_PI);
        int rnd_sign = (rng_.uniformInteger(0, 1) == 1) ? 1 : -1;

        double x_goal = currentBase.getOrigin().x() + goal_dist * cos(goal_orientation);
        double y_goal = currentBase.getOrigin().y() + ((double)rnd_sign) * goal_dist * sin(goal_orientation);
        double z_goal = rng_.uniformReal(min_goal_height, max_goal_height);
        currentGripperGOAL_.setOrigin(tf::Vector3(x_goal, y_goal, z_goal));

        tf::Quaternion q_goal;
        q_goal.setRPY(rng_.uniformReal(0.0, 2 * M_PI), rng_.uniformReal(0.0, 2 * M_PI), rng_.uniformReal(0.0, 2 * M_PI));
        currentGripperGOAL_.setRotation(q_goal.normalized());

        if (gripper_goal_distribution == "fixed") {
            throw std::runtime_error("Fixed gripper_goal_distribution not implemented anymore");
        }

        if (world_->get_name() == "world") {
            // ensure the goal is within our map
            valid = world_->is_within_world(currentGripperGOAL_);
            if (valid) {
                add_goal_marker_tf(currentGripperGOAL_, 99999, "pink");
                std::cout << "Next gripper goal in world coordinates: (" << x_goal << ", " << y_goal << ")." << std::endl;
                std::string accept = "";
                while ((accept != "a") && (accept != "n")) {
                    std::cout << "Press 'a' to accept, 'n' to try again: ";  // Type smth and press enter
                    std::cin >> accept;                                      // Get user input from the keyboard
                    std::cout << "Received input: " << accept << std::endl;
                    valid &= (accept == "a");
                }
            }
        } else {
            valid = true;
        }
    }

    currentBaseGOAL_ = currentGripperGOAL_;
    currentBaseGOAL_.setOrigin(tf::Vector3(currentGripperGOAL_.getOrigin().x(), currentGripperGOAL_.getOrigin().y(), 0.0));
    // reset joint_model_group_
    // kinematic_state_->setJointGroupPositions(joint_model_group_, current_joint_values_);
}

tf::Transform DynamicSystem_base::parse_goal(const std::vector<double> &gripper_goal) {
    tf::Quaternion rotation;
    if (gripper_goal.size() == 6) {
        rotation.setRPY(gripper_goal[3], gripper_goal[4], gripper_goal[5]);
    } else if (gripper_goal.size() == 7) {
        rotation = tf::Quaternion(gripper_goal[3], gripper_goal[4], gripper_goal[5], gripper_goal[6]);
    } else {
        throw std::runtime_error("invalid length of specified gripper goal");
    }
    return tf::Transform(rotation, tf::Vector3(gripper_goal[0], gripper_goal[1], gripper_goal[2]));
}

std::vector<double> DynamicSystem_base::set_gripper_goal(std::vector<double> gripper_goal,
                                                         std::string gripper_goal_distribution,
                                                         std::string gmm_model_path,
                                                         double success_thres_dist,
                                                         double success_thres_rot,
                                                         double start_pause) {
    success_thres_dist_ = success_thres_dist;
    success_thres_rot_ = success_thres_rot;
    start_pause_ = start_pause;

    // update the current state again before we start a new subgoal
    if (!world_->is_analytical()) {
        currentBaseTransform_ = world_->get_base_transform_world();
        // currently failling to call /get_planning_scene for hsr so just don't update
        update_current_gripper_from_world();
    }
    tf::Transform currentGripperGOAL_input;
    if (gripper_goal.empty()) {
        set_new_random_goal(gripper_goal_distribution);
        currentGripperGOAL_input = currentGripperGOAL_;
    } else {
        currentGripperGOAL_input = parse_goal(gripper_goal);
        // transform from a goal for the gripper tip into a goal for the specified gripper link
        currentGripperGOAL_ = utils::tip_to_gripper_goal(currentGripperGOAL_input, robo_config_.tip_to_gripper_offset, robo_config_.gripper_to_base_rot_offset);
    }

    // There's probably a better way than redefining the planner everytime
    // NOTE: IF ADJUSTING PLANNER VELOCITY CONSTRAINTS, ALSO ADJUST robo_config_.base_vel_rng, robo_config_.base_rot_rng (DON'T FORGET TIAGO)
    if (gmm_model_path != ""){
        // goal for gmm planner is origin of the object -> pass original goal input to planner, then change to wrist goal after instantiating, then call tip_to_gripper_goal() again
        gripper_planner_ = new GMMPlanner(robo_config_.tip_to_gripper_offset,
                                          robo_config_.gripper_to_base_rot_offset,
                                          currentGripperGOAL_input,
                                          currentGripperTransform_,
                                          currentBaseGOAL_,
                                          currentBaseTransform_,
                                          gmm_model_path,
                                          robo_config_.gmm_base_offset);
        currentGripperGOAL_ = gripper_planner_->get_last_attractor();
        currentGripperGOAL_ = utils::tip_to_gripper_goal(currentGripperGOAL_, robo_config_.tip_to_gripper_offset, robo_config_.gripper_to_base_rot_offset);

        // display the attractors of the gmm
        std::vector<tf::Transform> mus = gripper_planner_->get_mus();
        for (int i = 0; i < mus.size(); i++) {
            visualization_msgs::Marker m = utils::marker_from_transform(mus[i], "gmm_mus", "blue", 1.0, 0, robo_config_.frame_id);
            gripper_visualizer_.publish(m);
        }
    } else {
        gripper_planner_ = new LinearPlanner(currentGripperGOAL_, currentGripperTransform_, currentBaseGOAL_, currentBaseTransform_);
    }
    // plan velocities to be modulated and set in next step. Assumes currentGripperTransform_, currentGripperTransform_ and prev_gripper_plan_ have already been set
    set_goal_time_ = time_;
    bool pause_gripper = in_start_pause();
    double last_dt = update_time(pause_gripper);
    // loading gmm models can take a small bit of time, so set the time_planner_ to zero afterwards
    time_planner_ = 0.0;

    planned_gripper_vel_.init();
    planned_base_vel_.init();

    if (init_controllers_) {
        // only for evaluations on the tasks to check collisions with objects of the scene
        // relies on the gazebo scene plugin to make the planning scene available
        // we assume that the objects won't change after the call to reset()
        std::vector<std::string> allowed_collisions;
        allowed_collisions.push_back("pick_obj.link");
        allowed_collisions.push_back("target_shelf.Door");
        allowed_collisions.push_back("target_drawer.Drawer1");
        allowed_collisions.push_back("ground_plane.link");
        planning_scene::PlanningScenePtr scene = planning_scene_monitor_->getPlanningScene();
        // scene->getCurrentStateNonConst().update();
        setAllowedCollisionMatrix(scene, allowed_collisions, true);
    }

    visualization_msgs::Marker goal_input_marker = utils::marker_from_transform(currentGripperGOAL_input, "gripper_goal_input", utils::get_color_msg("blue"), marker_counter_, robo_config_.frame_id);
    gripper_visualizer_.publish(goal_input_marker);
    visualization_msgs::Marker goal_marker = utils::marker_from_transform(currentGripperGOAL_input, "gripper_goal", utils::get_color_msg("blue"), marker_counter_, robo_config_.frame_id);
    gripper_visualizer_.publish(goal_marker);

    return build_obs_vector(tf::Vector3(0, 0, 0), tf::Vector3(0, 0, 0), tf::Quaternion(0, 0, 0, 0));
}

void DynamicSystem_base::set_gripper_to_neutral() {
    // a) predefined neutral position
    kinematic_state_->setVariablePositions(robo_config_.neutral_pos_joint_names, robo_config_.neutral_pos_values);
    // update values
    const Eigen::Affine3d &end_effector_state = kinematic_state_->getGlobalLinkTransform(robo_config_.global_link_transform);
    tf::transformEigenToTF(end_effector_state, rel_gripper_pose_);
    currentGripperTransform_ = currentBaseTransform_ * rel_gripper_pose_;

    kinematic_state_->copyJointGroupPositions(joint_model_group_, current_joint_values_);
}

double DynamicSystem_base::draw_rng(double lower, double upper) {
    if (lower == upper) {
        return lower;
    } else if (lower > upper) {
        throw std::runtime_error("lower > upper");
    } else {
        return rng_.uniformReal(lower, upper);
    }
}

bool DynamicSystem_base::out_of_workspace(tf::Transform gripper_tf) {
    return (gripper_tf.getOrigin().z() < robo_config_.restricted_ws_z_min) || (gripper_tf.getOrigin().z() > robo_config_.restricted_ws_z_max);
}

// base_start: [xmin, xmax, ymin, ymax] or empty to use origin
bool DynamicSystem_base::set_start_pose(std::vector<double> base_start, std::string start_pose_distribution) {
    // Reset Base to origin
    double xbase = 0, ybase = 0, yawbase = 0;

    if (world_->get_name() == "world") {
        ROS_INFO("Real world execution set. Taking the current base transform as starting point.");
        currentBaseTransform_ = world_->get_base_transform_world();
    } else {
        if (!base_start.empty()) {
            if (base_start.size() != 6) { throw std::runtime_error("invalid length of specified base_start"); }
            xbase = draw_rng(base_start[0], base_start[1]);
            ybase = draw_rng(base_start[2], base_start[3]);
            yawbase = draw_rng(base_start[4], base_start[5]);
        }

        currentBaseTransform_.setOrigin(tf::Vector3(xbase, ybase, 0.0));
        tf::Quaternion q_base;
        q_base.setRPY(0.0, 0.0, yawbase);
        currentBaseTransform_.setRotation(q_base);
    }
    // Reset Gripper pose to start
    if (start_pose_distribution == "fixed") {
        set_gripper_to_neutral();
    } else if ((start_pose_distribution == "rnd") || (start_pose_distribution == "restricted_ws")) {
        // c) RANDOM pose relative to base
        collision_detection::CollisionRequest collision_request;
        collision_request.group_name = robo_config_.joint_model_group_name;
        collision_detection::CollisionResult collision_result;

        bool invalid = true;
        while (invalid) {
            kinematic_state_->setToRandomPositions(joint_model_group_, rng_);

            // check if in self-collision
            planning_scene_->getCurrentStateNonConst().update();

            robot_state::RobotState state_copy(*kinematic_state_);
            state_copy.setVariablePosition("world_joint/x", currentBaseTransform_.getOrigin().x());
            state_copy.setVariablePosition("world_joint/y", currentBaseTransform_.getOrigin().y());
            state_copy.setVariablePosition("world_joint/theta", currentBaseTransform_.getRotation().getAngle() * currentBaseTransform_.getRotation().getAxis().getZ());

            planning_scene_->checkCollisionUnpadded(collision_request, collision_result, state_copy);
            invalid = collision_result.collision;
            ROS_INFO_COND(collision_result.collision, "set_start_pose: drawn pose in self-collision, trying again");
            collision_result.clear();

            if (start_pose_distribution == "restricted_ws") {
                const Eigen::Affine3d &ee_pose = kinematic_state_->getGlobalLinkTransform(robo_config_.global_link_transform);
                tf::Transform temp_tf;
                tf::transformEigenToTF(ee_pose, temp_tf);
                invalid &= out_of_workspace(temp_tf);
                ROS_INFO_COND(invalid, "Goal outside of restricted ws, sampling again.");
            }
        }

        const Eigen::Affine3d &end_effector_state = kinematic_state_->getGlobalLinkTransform(robo_config_.global_link_transform);
        tf::transformEigenToTF(end_effector_state, rel_gripper_pose_);
        // multiplication theoretically unnecessary as long as currentBaseTransform_ is the identity
        currentGripperTransform_ = currentBaseTransform_ * rel_gripper_pose_;
        kinematic_state_->copyJointGroupPositions(joint_model_group_, current_joint_values_);
    } else {
        throw std::runtime_error("Invalid start_pose_distribution");
    }

    bool success = set_pose_in_world();
    return success;
}

// gripper_goal: [x, y, z, roll, pitch, yaw] or empty to draw a random goal
// base_start: [xmin, xmax, ymin, ymax] in meters
std::vector<double> DynamicSystem_base::reset(std::vector<double> gripper_goal,
                                              std::vector<double> base_start,
                                              std::string start_pose_distribution,
                                              std::string gripper_goal_distribution,
                                              bool do_close_gripper,
                                              std::string gmm_model_path,
                                              double success_thres_dist,
                                              double success_thres_rot,
                                              double start_pause,
                                              bool verbose) {
    ROS_INFO_COND(!world_->is_analytical(), "Reseting environment");

    ik_error_count_ = 0;
    verbose_ = verbose;
    // set start for both base and gripper
    // if not the analytical env, we actually execute it in gazebo to reset. This might sometimes fail. So continue sampling a few random poses
    bool success = false;
    int trials = 0, max_trials = 50;
    while ((!success) && trials < max_trials) {
        success = set_start_pose(base_start, start_pose_distribution);
        trials++;
    }
    if (trials > max_trials) {
        throw std::runtime_error("Could not set start pose after 50 trials!!!");

//        ROS_WARN("Could not set start pose after 50 trials!!! Calling /gazebo/reset_world and trying again.");
//        ros::ServiceClient reset_client = nh_->serviceClient<std_srvs::Empty>("/gazebo/reset_world");
//        std_srvs::Empty emptySrv;
//        reset_client.call(emptySrv);
//        ros::Duration(3.0).sleep();
//        trials = 0;
//        while (!success){
//            success = set_start_pose(base_start, start_pose_distribution);
//            trials++;
//            if (trials > max_trials){ throw std::runtime_error("Could not set start pose after 50 trials!!!"); }
//        }
    }

    if (do_close_gripper) {
        close_gripper(0.0, false);
    }

    // reset time after the start pose is set
    time_ = (world_->is_analytical()) ? 0.0 : ros::Time::now().toSec();
    reset_time_ = time_;

    // Clear the visualizations
    visualization_msgs::Marker marker;
    marker.header.frame_id = robo_config_.frame_id;
    marker.header.stamp = ros::Time::now();
    marker.action = visualization_msgs::Marker::DELETEALL;
    gripper_visualizer_.publish(marker);

    display_trajectory_.trajectory.clear();
    pathPoints_.clear();
    gripper_plan_marker_.markers.clear();
    marker_counter_++;
    if (marker_counter_ > 3) {
        marker_counter_ = 0;
    }

    // Set new random goals for base and gripper. Assumes that we've already set the currentBaseTransform_, currentGripperTransform_
    // also sets the plan for the first step
    std::vector<double> obs = set_gripper_goal(gripper_goal, gripper_goal_distribution, gmm_model_path, success_thres_dist, success_thres_rot, start_pause);

    add_trajectory_point(gripper_planner_->get_prev_plan(), true);

    // return observation vector
    return obs;
}

// easiest way to know the dim without having to enforce that everything is already initialised
int DynamicSystem_base::get_obs_dim() {
    return 22 + joint_names_.size();
}

// Build the observation vector
std::vector<double> DynamicSystem_base::build_obs_vector(tf::Vector3 current_planned_base_vel_world,
                                                         tf::Vector3 current_planned_gripper_vel_world,
                                                         tf::Quaternion current_planned_gripper_vel_dq) {
    std::vector<double> obs_vector;
    // whether to represent rotations as quaternions or euler angles
    bool use_euler = false;

    // gripper state relative to the base
    utils::add_vector3(obs_vector, rel_gripper_pose_.getOrigin());
    utils::add_rotation(obs_vector, rel_gripper_pose_.getRotation(), use_euler);

    // always provide the RL agent with the velocities normed to the time step used in training
    GripperPlan next_plan_training = gripper_planner_->get_next_velocities(
        time_planner_ / slow_down_factor_,
        in_start_pause() ? 0.0 : time_step_train_,  // NOTE: should we include slow_down_factor_ here as well? -> SEEMS TO REDUCE PERFORMANCE FOR RELVEL, DIRVEL DOESN'T CARE
        currentBaseTransform_,
        currentGripperTransform_,
        current_planned_base_vel_world,
        current_planned_gripper_vel_world,
        current_planned_gripper_vel_dq,
        conf::min_planner_velocity,
        conf::max_planner_velocity,
        false);

    // next planned gripper velocity
    // Pass as obs the unconstrained velocities. For execution we will scale them into [min_planner_velocity_, max_planner_velocity_] range
    PlannedVelocities planned_gripper_vel = gripper_planner_->transformToVelocity(currentGripperTransform_, next_plan_training.nextGripperTransform, currentBaseTransform_, 0.0);
    utils::add_vector3(obs_vector, planned_gripper_vel.vel_rel);

    // planned change in rotation
    utils::add_rotation(obs_vector, planned_gripper_vel.dq, use_euler);

    // relative position of the gripper goal
    tf::Transform rel_gripper_goal = currentBaseTransform_.inverse() * currentGripperGOAL_;
    utils::add_vector3(obs_vector, rel_gripper_goal.getOrigin());
    utils::add_rotation(obs_vector, rel_gripper_goal.getRotation(), use_euler);

    // legacy to ensure compatibility of the trained checkpoints
    obs_vector.push_back(0.0);

    // current joint positions (8 values)
    for (int j = 0; j < current_joint_values_.size(); j++) {
        obs_vector.push_back(current_joint_values_[j]);
    }

    if (obs_vector.size() != get_obs_dim()) {
        throw std::runtime_error("get_obs_dim returning wrong value. Pls update.");
    }
    return obs_vector;
}

// NOTE: the other parts of the reward (action regularization) happens in python
double DynamicSystem_base::calc_reward(bool found_ik, double regularization) {
    double reward = -penalty_scaling_ * regularization;
    if (!found_ik) {
        reward -= 1.0;
    }
    return reward;
}

double DynamicSystem_base::get_dist_to_goal() {
    return (currentGripperGOAL_.getOrigin() - currentGripperTransform_.getOrigin()).length();
}

double DynamicSystem_base::get_rot_dist_to_goal() {
    return utils::calc_rot_dist(currentGripperTransform_, currentGripperGOAL_);
}

int DynamicSystem_base::calc_done_ret(bool found_ik, int max_allow_ik_errors) {
    // alternative: get a signal from the gripper trajectory planner that we are at the end
    int done_return = 0;
    if (ik_error_count_ > max_allow_ik_errors) {
        done_return = 2;
    } else if (!found_ik) {
        done_return = 0;
    } else {
        double dist_to_goal = get_dist_to_goal();
        bool is_close = (dist_to_goal < success_thres_dist_);

        if (is_close) {
            // distance to target rotation: https://math.stackexchange.com/questions/90081/quaternion-distance
            double rot_distance = utils::calc_rot_dist(currentGripperTransform_, currentGripperGOAL_);
            // more exact alternative; seems to have some precision problems, returning nan if slightly above 1
            // double rot_distance = inner_prod > 1.0 ? 0.0 : acos(2.0 * pow(inner_prod, 2.0) - 1.0);
            is_close &= (rot_distance < success_thres_rot_);
        }
        done_return = is_close ? 1 : 0;
    }
    ROS_INFO_COND((done_return != 0) && (!world_->is_analytical()), "Episode finished with done_return %d and %d ik fails", done_return, ik_error_count_);
    return done_return;
}

bool DynamicSystem_base::find_ik(const Eigen::Isometry3d &desiredState, const tf::Transform &desiredGripperTfWorld) {
    // kinematics::KinematicsQueryOptions ik_options;
    // ik_options.return_approximate_solution = true;
    if (perform_collision_check_) {
        bool success = kinematic_state_->setFromIK(joint_model_group_, desiredState, 0.05, constraint_callback_fn_);
        if (!success) {
            // in case of a collision keep the current position
            // can apply this to any case of ik failure as moveit does not seem to set it to the next best solution anyway
            kinematic_state_->setJointGroupPositions(robo_config_.joint_model_group_name, current_joint_values_);
        }
        return success;
    } else {
        // return kinematic_state_->setFromIK(joint_model_group_, desiredState, 5, 0.1, moveit::core::GroupStateValidityCallbackFn(), ik_options);
        return kinematic_state_->setFromIK(joint_model_group_, desiredState, 0.05);
    }
}

geometry_msgs::Twist DynamicSystem_base::calc_desired_base_transform(std::vector<double> &base_actions,
                                                                     tf::Vector3 planned_base_vel_rel,
                                                                     tf::Quaternion planned_base_q,
                                                                     tf::Vector3 planned_gripper_vel_rel,
                                                                     tf::Transform &desiredBaseTransform,
                                                                     double transition_noise_base,
                                                                     double &regularization,
                                                                     double const &last_dt,
                                                                     const tf::Transform &desiredGripperTransform) {
    planned_base_vel_rel *= slow_down_factor_;
    planned_gripper_vel_rel *= slow_down_factor_;

    // a) calculate the new desire baseTransform
    // planner actions are based on last_dt, RL actions are for a unit time -> scale down RL actions
    double base_rot_rng_t = last_dt * robo_config_.base_rot_rng;
    double base_vel_rng_t = last_dt * robo_config_.base_vel_rng;

    // Modulate planned base velocity and set it:
    // i) derive actions from agent's actions
    tf::Vector3 base_vel_rel;
    double base_rotation = 0.0;

    if (strategy_ == "modulate_ellipse") {
        // Need velocities in world frame
        tf::Transform base_no_trans = currentBaseTransform_;
        base_no_trans.setOrigin(tf::Vector3(0.0, 0.0, 0.0));

        tf::Vector3 base_vel_wf = base_no_trans * planned_base_vel_rel;
        tf::Vector3 gripper_vel_wf = base_no_trans * planned_gripper_vel_rel;

        Eigen::VectorXf combined_pose(14);
        Eigen::VectorXf combined_speed(14);
        float base_rot_speed = 0.0001;
        combined_speed << gripper_vel_wf.x(), gripper_vel_wf.y(), 0.0, 0.0, 0.0, 0.0, 0.0,
                          base_vel_wf.x(), base_vel_wf.y(), 0.0, 0.0, 0.0, base_rot_speed, 0.0;
        combined_pose << desiredGripperTransform.getOrigin().x(),desiredGripperTransform.getOrigin().y(), desiredGripperTransform.getOrigin().z(),
                         desiredGripperTransform.getRotation().x(),desiredGripperTransform.getRotation().y(),desiredGripperTransform.getRotation().z(),desiredGripperTransform.getRotation().w(),
                         currentBaseTransform_.getOrigin().x(),currentBaseTransform_.getOrigin().y(), currentBaseTransform_.getOrigin().z(),
                         currentBaseTransform_.getRotation().x(),currentBaseTransform_.getRotation().y(),currentBaseTransform_.getRotation().z(),currentBaseTransform_.getRotation().w();
        modulation_.run(combined_pose, combined_speed);

        // Transform back to robot frame
        tf::Vector3 base_vel_rf = base_no_trans.inverse() * tf::Vector3(combined_speed(7), combined_speed(8), 0.0);
        base_vel_rel.setValue(base_vel_rf.x(), base_vel_rf.y(), 0.0);
        base_rotation = utils::clamp_double(combined_speed(12) * 10.0, -base_rot_rng_t, base_rot_rng_t);

        visualization_msgs::MarkerArray ma = modulation_.getEllipsesVisMarker(combined_pose, combined_speed);
        ellipses_pub_.publish(ma);
    } else if (strategy_ == "unmodulated") {
        base_vel_rel.setValue(planned_base_vel_rel.x(),
                              planned_base_vel_rel.y(),
                              // add the gripper z to ensure the base does not outrun the gripper after norm_scale_vel() due to having z of 0
                              planned_gripper_vel_rel.z());

        double roll_, pitch_, yaw_, yaw2_;
        tf::Matrix3x3(planned_base_q).getRPY(roll_, pitch_, yaw_);
        tf::Matrix3x3(currentBaseTransform_.getRotation()).getRPY(roll_, pitch_, yaw2_);
        double angle_diff = utils::rpy_angle_diff(yaw_, yaw2_);
        base_rotation = utils::clamp_double(angle_diff, -base_rot_rng_t, base_rot_rng_t);
    } else if ((strategy_ == "relvelm") || (strategy_ == "relveld")) {
        // ALTERNATIVE WOULD BE TO INTERPRET modulation_lambda1 AS VELOCITY AND modulation_lambda2 AS ANGLE, THEN MOVE VEL * COS(X) AND VEL * SIN(X). See Tiago.
        double dx = base_vel_rng_t * base_actions[1];
        double dy = base_vel_rng_t * base_actions[2];
        base_vel_rel.setValue(planned_gripper_vel_rel.x() + dx,
                              planned_gripper_vel_rel.y() + dy,
                              0.0);
        base_rotation = base_rot_rng_t * base_actions[0];

        if (strategy_ == "relvelm") {
            // a) modulate as little as possible
            regularization += pow(base_actions[0], 2.0) + pow(base_actions[1], 2.0) + pow(base_actions[2], 2.0);
        } else {
            // b) keep total speed low (scaled back up into -1, 1 range)
            double denom = std::abs(base_vel_rng_t) < 0.000001 ? 1.0 : base_vel_rng_t;
            regularization += pow(base_actions[0], 2.0) + pow(base_vel_rel.length() / denom, 2.0);
        }
    } else if (strategy_ == "dirvel") {
        double dx = base_vel_rng_t * base_actions[1];
        double dy = base_vel_rng_t * base_actions[2];
        base_vel_rel.setValue(dx, dy, 0.0);
        base_rotation = base_rot_rng_t * base_actions[0];

        regularization += pow(base_actions[0], 2.0) + pow(base_actions[1], 2.0) + pow(base_actions[2], 2.0);
    } else {
        throw std::runtime_error("Unimplemented strategy");
    }

    // ensure the velocity limits are still satisfied
    base_vel_rel = utils::norm_scale_vel(base_vel_rel, 0.0, base_vel_rng_t);
    // ensure z component is 0 (relevant for 'hack' in unmodulated strategy)
    base_vel_rel.setZ(0.0);

    if (transition_noise_base > 0.0001) {
        tf::Vector3 noise_vec = tf::Vector3(rng_.gaussian(0.0, transition_noise_base), rng_.gaussian(0.0, transition_noise_base), 0.0);
        base_vel_rel += noise_vec;
        base_rotation += rng_.gaussian(0.0, transition_noise_base);
    }

    // ii) set corresponding new base speed
    desiredBaseTransform = currentBaseTransform_;

    tf::Transform base_no_trans = currentBaseTransform_;
    base_no_trans.setOrigin(tf::Vector3(0.0, 0.0, 0.0));
    // from robot-base reference frame back to global reference frame
    tf::Vector3 base_vel = base_no_trans * base_vel_rel;
    desiredBaseTransform.setOrigin(currentBaseTransform_.getOrigin() + base_vel);

    // iii) rotate base
    tf::Quaternion q(tf::Vector3(0.0, 0.0, 1.0), base_rotation);
    desiredBaseTransform.setRotation(q * currentBaseTransform_.getRotation());

    // construct base command: scale back up to be per unit time
    double cmd_scaling = 1.0;
    if (last_dt > 0.001) {
        cmd_scaling /= last_dt;
    }

    tf::Transform relative_desired_pose = currentBaseTransform_.inverse() * desiredBaseTransform;
    geometry_msgs::Twist base_cmd_rel;
    double roll_, pitch_, yaw;
    relative_desired_pose.getBasis().getRPY(roll_, pitch_, yaw);
    base_cmd_rel.linear.x = relative_desired_pose.getOrigin().getX() * cmd_scaling;
    base_cmd_rel.linear.y = relative_desired_pose.getOrigin().getY() * cmd_scaling;
    base_cmd_rel.angular.z = yaw * cmd_scaling;

    //  same values as above. But won't easily adapt to inprecisions in the actual base transform I guess
    // scale to unit speed and ensure constraints are held
    // base_vel_rel = utils::norm_scale_vel(cmd_scaling * base_vel_rel, 0.0, robo_config_.base_vel_rng);
    // geometry_msgs::Twist base_cmd_rel2;
    // base_cmd_rel2.linear.x = base_vel_rel.x();
    // base_cmd_rel2.linear.y = base_vel_rel.y();
    // base_cmd_rel2.linear.z = base_vel_rel.z();
    // base_cmd_rel2.angular.z = base_rotation * cmd_scaling;
    // std::cout << "base_cmd_rel2.linear.x: " << base_cmd_rel.linear.x << " , base_cmd_rel2.linear.y: " << base_cmd_rel.linear.y << " , base_cmd_rel2.angular.z: " << base_cmd_rel.angular.z << std::endl;

    return base_cmd_rel;
}

double DynamicSystem_base::update_time(bool pause_gripper) {
    double dt;
    if ((!world_->is_analytical())) {
        dt = (ros::Time::now().toSec() - time_);
        // assume we call it in exactly the expected frequency?
        // dt = rate_.expectedCycleTime().toSec();
    } else {
        dt = time_step_train_;
    }

    time_ += dt;
    if (!pause_gripper) {
        time_planner_ += dt;
    }
    // ROS_INFO("t-: %f, tp: %f, dt: %f, sp: %f", time_ - reset_time_, time_planner_, dt, start_pause_);
    return dt;
}

bool DynamicSystem_base::in_start_pause() {
    return (time_ - set_goal_time_) < start_pause_;
}

std::vector<double> DynamicSystem_base::step(int max_allow_ik_errors,
                                             std::vector<double> base_actions,
                                             double transition_noise_ee,
                                             double transition_noise_base) {
    PathPoint path_point;
    bool pause_gripper = in_start_pause();

    // utils::print_t(currentGripperTransform_, "currentGripperTransform_");
    // utils::print_t(next_plan.nextGripperTransform, "next_plan.nextGripperTransform");

    int action_repeat = (world_->is_analytical()) ? 1 : (time_step_real_exec_ / rate_.expectedCycleTime().toSec());

    tf::Transform desiredGripperTransform, desiredBaseTransform, desired_gripper_pose_rel;
    GripperPlan next_plan;
    geometry_msgs::Twist base_cmd_rel;
    bool found_ik;
    bool collision = false;
    double regularization = 0.0, last_dt;

    for (int i = 0; i < action_repeat; i++) {
        // plan velocities to be modulated and set in next step
        last_dt = update_time(pause_gripper);
        next_plan = gripper_planner_->get_next_velocities(time_planner_ / slow_down_factor_,
                                                          last_dt / slow_down_factor_,
                                                          currentBaseTransform_,
                                                          currentGripperTransform_,
                                                          planned_base_vel_.vel_world,
                                                          planned_gripper_vel_.vel_world,
                                                          planned_gripper_vel_.dq,
                                                          conf::min_planner_velocity,
                                                          conf::max_planner_velocity,
                                                          !pause_gripper);
        if (transition_noise_ee > 0.0001) {
            tf::Vector3 noise_vec = tf::Vector3(rng_.gaussian(0.0, transition_noise_ee), rng_.gaussian(0.0, transition_noise_ee), rng_.gaussian(0.0, transition_noise_ee));
            next_plan.nextGripperTransform.setOrigin(next_plan.nextGripperTransform.getOrigin() + noise_vec);
        }
        if (i == 0) {
            utils::pathPoint_insert_transform(path_point, "planned_gripper", next_plan.nextGripperTransform);
            utils::pathPoint_insert_transform(path_point, "planned_base", next_plan.nextBaseTransform, true);
        }

        // constrain by base_vel_rng, not gripper planner max vel so that we could theoretically still catch up
        // must come before we update the currentGripperTransform_
        // even if pause_gripper, still calculate the originally planned one, because with relvel we still want to move the base relative to this originally planned speed(?)
        planned_gripper_vel_ = gripper_planner_->transformToVelocity(currentGripperTransform_,
                                                                     next_plan.nextGripperTransform,
                                                                     currentBaseTransform_,
                                                                     robo_config_.base_vel_rng);
        planned_base_vel_ = gripper_planner_->transformToVelocity(currentBaseTransform_,
                                                                  next_plan.nextBaseTransform,
                                                                  currentBaseTransform_,
                                                                  robo_config_.base_vel_rng);

        if (pause_gripper) {
            next_plan = gripper_planner_->get_prev_plan();
        }
        // set new gripper pose (optimistically assume it will be achieved, updating it again after trying to execute the ik)
        desiredGripperTransform = next_plan.nextGripperTransform;

        // apply the RL actions to the base, updating desiredBaseTransform while holding the velocity constraints
        base_cmd_rel = calc_desired_base_transform(base_actions,
                                                   planned_base_vel_.vel_rel,
                                                   next_plan.nextBaseTransform.getRotation(),
                                                   planned_gripper_vel_.vel_rel,
                                                   desiredBaseTransform,
                                                   transition_noise_base,
                                                   regularization,
                                                   last_dt,
                                                   desiredGripperTransform);
        // std::cout << "base_cmd_rel.linear.x: " << base_cmd_rel.linear.x << " , base_cmd_rel.linear.y: " << base_cmd_rel.linear.y << " , base_cmd_rel.angular.z: " <<
        // base_cmd_rel.angular.z << std::endl;

        // Update relative positions of the base, gripper and gripper_goal to the base (optimistically assume it will be achieved, updating it again after trying to execute the ik)
        desired_gripper_pose_rel = desiredBaseTransform.inverse() * desiredGripperTransform;
        //if (world_->is_analytical()){
        //    desired_gripper_pose_rel = desiredBaseTransform.inverse() * desiredGripperTransform;
        //} else {
        //    desired_gripper_pose_rel = currentBaseTransform_.inverse() * desiredGripperTransform;
        //}
        gripper_visualizer_.publish(
            create_vel_marker(currentGripperTransform_, 20 * (desiredGripperTransform.getOrigin() - currentGripperTransform_.getOrigin()), "gripper_vel", "cyan", 0));
        gripper_visualizer_.publish(create_vel_marker(currentBaseTransform_, 20 * (desiredBaseTransform.getOrigin() - currentBaseTransform_.getOrigin()), "base_vel", "cyan", 0));

        // Perform IK checks
        Eigen::Isometry3d state;
        tf::poseTFToEigen(desired_gripper_pose_rel, state);
        const Eigen::Isometry3d &desiredState = state;
        found_ik = find_ik(desiredState, desiredGripperTransform);
        kinematic_state_->copyJointGroupPositions(joint_model_group_, current_joint_values_);

        if ((!world_->is_analytical())) {
            rate_.sleep();
            send_arm_command(current_joint_values_, 0.1);
            cmd_base_vel_pub_.publish(base_cmd_rel);
        };

        if (!found_ik) {
            ik_error_count_++;
            // planning_scene_monitor_->getPlanningScene()->getCurrentState().copyJointGroupPositions(joint_model_group_, current_joint_values_);
        }
        // update state to what we actually achieve
        // a) base: without execution we'll always be at the next base transform
        if (world_->is_analytical()) {
            currentBaseTransform_ = desiredBaseTransform;
        } else {
            currentBaseTransform_ = world_->get_base_transform_world();
        }
        // b) gripper: update kinematic state from planning scene and run forward kinematics to get achieved currentGripperTransform_
        const Eigen::Affine3d &end_effector_state_rel = kinematic_state_->getGlobalLinkTransform(robo_config_.global_link_transform);
        tf::transformEigenToTF(end_effector_state_rel, rel_gripper_pose_);
        currentGripperTransform_ = currentBaseTransform_ * rel_gripper_pose_;
        // update_current_gripper_from_world();

        // there seems to be an incompatibility with some geometries leading to occasional segfaults within planning_scene::PlanningScene::checkCollisionUnpadded
        // if (init_controllers_){
        //     collision |= check_scene_collisions();
        // }

        add_trajectory_point(next_plan, found_ik);
    }

    // reward and check if episode has finished -> Distance gripper to goal
    // found_ik &= get_arm_success();
    double reward = calc_reward(found_ik, regularization);
    int done_ret = calc_done_ret(found_ik, max_allow_ik_errors);

    // build the observation return
    std::vector<double> obs_vector = build_obs_vector(planned_base_vel_.vel_world, planned_gripper_vel_.vel_world, planned_gripper_vel_.dq);
    obs_vector.push_back(reward);
    obs_vector.push_back(done_ret);
    obs_vector.push_back(ik_error_count_);

    // visualisation etc
    utils::pathPoint_insert_transform(path_point, "base", currentBaseTransform_, true);
    utils::pathPoint_insert_transform(path_point, "desired_base", desiredBaseTransform, true);
    path_point["base_cmd_linear_x"] = base_cmd_rel.linear.x;
    path_point["base_cmd_linear_y"] = base_cmd_rel.linear.y;
    path_point["base_cmd_angular_z"] = base_cmd_rel.angular.z;
    utils::pathPoint_insert_transform(path_point, "gripper", currentGripperTransform_);
    utils::pathPoint_insert_transform(path_point, "gripper_rel", rel_gripper_pose_);
    utils::pathPoint_insert_transform(path_point, "desired_gripper_rel", desired_gripper_pose_rel);
    path_point["ik_fail"] = !found_ik;
    path_point["dt"] = last_dt;
    path_point["collision"] = collision;
    pathPoints_.push_back(path_point);

    return obs_vector;
}

std_msgs::ColorRGBA DynamicSystem_base::get_ik_color(double alpha = 1.0) {
    std_msgs::ColorRGBA c;
    // more and more red from 0 to 100
    double ik_count_capped = std::min((double)ik_error_count_, 100.0);
    c.r = (ik_count_capped / 100.0);
    c.g = ((1.0 - c.r));
    c.b = 0.0;
    c.a = alpha;
    return c;
}

void DynamicSystem_base::add_trajectory_point(const GripperPlan &next_plan, bool found_ik) {
    if (!verbose_) {
        return;
    }
    // plans
    double nthpoint = (world_->is_analytical()) ? (1.0 / time_step_train_) : (1.0 / (time_step_real_exec_));
    if (((pathPoints_.size() % (int)nthpoint) == 0) || !found_ik) {
        int mid = 5000 * marker_counter_ + gripper_plan_marker_.markers.size();
        visualization_msgs::Marker marker = utils::marker_from_transform(next_plan.nextGripperTransform, "gripper_plan", get_ik_color(0.5), mid, robo_config_.frame_id);
        gripper_visualizer_.publish(marker);
        gripper_plan_marker_.markers.push_back(marker);

        visualization_msgs::Marker base_plan_marker = utils::marker_from_transform(next_plan.nextBaseTransform, "base_plan", "orange", 0.5, mid, robo_config_.frame_id);
        gripper_visualizer_.publish(base_plan_marker);

        visualization_msgs::Marker base_marker = utils::marker_from_transform(currentBaseTransform_, "base_actual", "yellow", 0.5, mid, robo_config_.frame_id);
        gripper_visualizer_.publish(base_marker);
    };

    // current robot state
    nthpoint = (world_->is_analytical()) ? 1 : (time_step_train_ / (time_step_real_exec_));
    if ((pathPoints_.size() % (int)nthpoint) == 0) {
        moveit_msgs::DisplayRobotState drs;
        robot_state::RobotState state_copy(*kinematic_state_);
        state_copy.setVariablePosition("world_joint/x", currentBaseTransform_.getOrigin().x());
        state_copy.setVariablePosition("world_joint/y", currentBaseTransform_.getOrigin().y());
        state_copy.setVariablePosition("world_joint/theta", currentBaseTransform_.getRotation().getAngle() * currentBaseTransform_.getRotation().getAxis().getZ());
        robot_state::robotStateToRobotStateMsg(state_copy, drs.state);
        // rviz won't accept these in map frame for some reason
        // drs.state.joint_state.header.frame_id = "map";
        // drs.state.multi_dof_joint_state.header.frame_id = "map";
        robstate_visualizer_.publish(drs);
    }
    // trajectory
    moveit_msgs::RobotTrajectory fullBodyTraj_msg;
    fullBodyTraj_msg.multi_dof_joint_trajectory.header.frame_id = robo_config_.frame_id;
    fullBodyTraj_msg.multi_dof_joint_trajectory.header.stamp = ros::Time(time_ - reset_time_);
    fullBodyTraj_msg.multi_dof_joint_trajectory.joint_names.push_back("world_joint");
    // arm trajectory point
    trajectory_msgs::JointTrajectoryPoint jointPoint;
    for (int j = 0; j < joint_names_.size(); j++) {
        fullBodyTraj_msg.joint_trajectory.joint_names.push_back(joint_names_[j]);
        jointPoint.positions.push_back(current_joint_values_[j]);
        // std::cout << joint_names_[j] << ": " << current_joint_values_[j] << std::endl;
    }
    fullBodyTraj_msg.joint_trajectory.points.push_back(jointPoint);
    // base
    trajectory_msgs::MultiDOFJointTrajectoryPoint basePoint;
    geometry_msgs::Transform transform;
    tf::transformTFToMsg(currentBaseTransform_, transform);
    transform.translation.z = 0;
    basePoint.transforms.push_back(transform);
    fullBodyTraj_msg.multi_dof_joint_trajectory.points.push_back(basePoint);

    display_trajectory_.trajectory.push_back(fullBodyTraj_msg);
}

std::vector<PathPoint> DynamicSystem_base::visualize_robot_pose(std::string logfile) {
    // Visualize the current gripper goal in color of
    visualization_msgs::Marker goal_marker = utils::marker_from_transform(currentGripperGOAL_, "gripper_goal", "blue", 1.0, marker_counter_, robo_config_.frame_id);

    // publish messages
    traj_visualizer_.publish(display_trajectory_);
    // gripper_visualizer_.publish(goal_marker);

    // Store in rosbag
    if (logfile != "") {
        logfile = logfile + "_nik" + std::to_string(ik_error_count_) + ".bag";
        ros::Time timeStamp = ros::Time::now();
        if (timeStamp.toNSec() == 0)
            timeStamp = ros::TIME_MIN;

        rosbag::Bag bag;
        bag.open(logfile, rosbag::bagmode::Write);
        bag.write("modulation_rl_ik/traj_visualizer", timeStamp, display_trajectory_);
        bag.write("modulation_rl_ik/gripper_goal_visualizer", timeStamp, goal_marker);
        bag.write("modulation_rl_ik/gripper_plan_visualizer", timeStamp, gripper_plan_marker_);
        bag.close();
    }

    return pathPoints_;
}

void DynamicSystem_base::add_goal_marker_tf(tf::Transform transfm, int marker_id, std::string color) {
    std::vector<double> pos;
    pos.push_back(transfm.getOrigin().x());
    pos.push_back(transfm.getOrigin().y());
    pos.push_back(transfm.getOrigin().z());
    pos.push_back(transfm.getRotation().x());
    pos.push_back(transfm.getRotation().y());
    pos.push_back(transfm.getRotation().z());
    pos.push_back(transfm.getRotation().w());
    add_goal_marker(pos, marker_id, color);
}

// currently won't be added to the rosbag. But the task will always set them at env initialization. So always visible
// pos: [x, y, z, R, P, Y] or [x, y, z, Qx, Qy, Qz, Qw]
void DynamicSystem_base::add_goal_marker(std::vector<double> pos, int marker_id, std::string color) {
    tf::Transform t = parse_goal(pos);
    std_msgs::ColorRGBA c = utils::get_color_msg(color, 1.0);
    visualization_msgs::Marker marker = utils::marker_from_transform(t, "gripper_goal", c, marker_id, robo_config_.frame_id);
    gripper_visualizer_.publish(marker);
}

// pybind will complain if pure virtual here
void DynamicSystem_base::open_gripper(double position, bool wait_for_result) {
    throw std::runtime_error("NOT IMPLEMENTED YET");
}

void DynamicSystem_base::close_gripper(double position, bool wait_for_result) {
    throw std::runtime_error("NOT IMPLEMENTED YET");
}

void DynamicSystem_base::update_current_gripper_from_world() {
    if ((!world_->is_analytical())) {
        // update kinematic_state_ and current_joint_values_
        moveit_msgs::GetPlanningScene scene_srv1;
        moveit_msgs::PlanningScene currentScene;
        scene_srv1.request.components.components = 2;  // moveit_msgs::PlanningSceneComponents::ROBOT_STATE;
        if (!client_get_scene_.call(scene_srv1)) {
            ROS_WARN("Failed to call service /get_planning_scene");
        }
        planning_scene_->setPlanningSceneDiffMsg(scene_srv1.response.scene);

        // planning_scene::PlanningScenePtr scene = planning_scene_monitor_->getPlanningScene();
        robot_state::RobotState robstate = planning_scene_->getCurrentState();
        // const std::vector< std::string > &all_joint_names = kinematic_model->getJointModelNames();
        // const std::vector< std::string > &all_joint_names = robstate.getVariableNames();
        for (int j = 0; j < joint_names_.size(); j++) {
            const std::string name = joint_names_[j];
            const double curr_value = kinematic_state_->getJointPositions(name)[0];
            const double actual_value = robstate.getJointPositions(name)[0];

            // avoid adding joints that are not defined in other places (e.g. rviz)
            if (std::abs(curr_value - actual_value) > 0.0000001) {
                kinematic_state_->setJointPositions(name, &actual_value);
            }
        }
        kinematic_state_->copyJointGroupPositions(joint_model_group_, current_joint_values_);
    }

    const Eigen::Affine3d &end_effector_state_rel = kinematic_state_->getGlobalLinkTransform(robo_config_.global_link_transform);
    tf::transformEigenToTF(end_effector_state_rel, rel_gripper_pose_);
    currentGripperTransform_ = currentBaseTransform_ * rel_gripper_pose_;
}

bool DynamicSystem_base::set_pose_in_world() {
    // set base
    world_->set_model_state(robo_config_.name, currentBaseTransform_, robo_config_, cmd_base_vel_pub_);
    // arm: use controllers
    bool success = true;
    if (!world_->is_analytical()) {
        ROS_INFO("Setting gripper to start");
        send_arm_command(current_joint_values_, 5.0);
        success = get_arm_success();
        ROS_WARN_COND(!success, "couldn't set arm to selected start pose");
    }
    return success;
}

void DynamicSystem_base::setAllowedCollisionMatrix(planning_scene::PlanningScenePtr planning_scene, std::vector<std::string> obj_names, bool allow) {
    acm_ = planning_scene->getAllowedCollisionMatrix();

    std::vector<std::string> all_names;
    acm_.getAllEntryNames(all_names);

    moveit_msgs::PlanningScene planning_scene_msg;
    planning_scene->getPlanningSceneMsg(planning_scene_msg);

    for (int i = 0; i < (int)planning_scene_msg.world.collision_objects.size(); ++i) {
        if (std::find(obj_names.begin(), obj_names.end(), planning_scene_msg.world.collision_objects[i].id) != obj_names.end()) {
            // ROS_INFO("Setting all collisions with %s to %d. ", planning_scene_msg.world.collision_objects[i].id.c_str(), allow);
            // acm_.setDefaultEntry(planning_scene_msg.world.collision_objects[i].id, allow);
            for (auto name : all_names) {
                acm_.setEntry(planning_scene_msg.world.collision_objects[i].id, name, allow);
                acm_.setEntry(name, planning_scene_msg.world.collision_objects[i].id, allow);
            }
        } else {
            // ROS_INFO("Setting all collisions with %s to %d. ", planning_scene_msg.world.collision_objects[i].id.c_str(), !allow);
            // acm_.setDefaultEntry(planning_scene_msg.world.collision_objects[i].id, !allow);
            for (auto name : all_names) {
                acm_.setEntry(planning_scene_msg.world.collision_objects[i].id, name, !allow);
                acm_.setEntry(name, planning_scene_msg.world.collision_objects[i].id, !allow);
            }
        }
    }
}

bool DynamicSystem_base::check_scene_collisions() {
    planning_scene::PlanningScenePtr planning_scene = planning_scene_monitor_->getPlanningScene();

    // change it only on a copy!
    robot_state::RobotState state_copy(*kinematic_state_);
    state_copy.setVariablePosition("world_joint/x", currentBaseTransform_.getOrigin().x());
    state_copy.setVariablePosition("world_joint/y", currentBaseTransform_.getOrigin().y());
    state_copy.setVariablePosition("world_joint/theta", currentBaseTransform_.getRotation().getAngle() * currentBaseTransform_.getRotation().getAxis().getZ());

    collision_detection::CollisionRequest collision_request;
    if (robo_config_.scene_collision_group_name != "") {
        collision_request.group_name = robo_config_.scene_collision_group_name;
    }
    collision_detection::CollisionResult collision_result;
    // planning_scene->setCurrentState(*kinematic_state_);
    // planning_scene->checkCollision(collision_request, collision_result, *kinematic_state_, acm_);
    planning_scene->checkCollisionUnpadded(collision_request, collision_result, state_copy, acm_);

    if (collision_result.collision) {
        // ROS_WARN("Collision with scene! N collisions: %d", collision_result.contacts.size());
        collision_detection::CollisionResult::ContactMap::const_iterator it;
        for (it = collision_result.contacts.begin(); it != collision_result.contacts.end(); ++it) {
            ROS_INFO("Contact between: %s and %s", it->first.first.c_str(), it->first.second.c_str());
        }
        return true;
    }
    return false;
}

visualization_msgs::Marker DynamicSystem_base::create_vel_marker(tf::Transform current_tf, tf::Vector3 vel, std::string ns, std::string color, int marker_id) {
    visualization_msgs::Marker marker;
    marker.header.frame_id = robo_config_.frame_id;
    marker.header.stamp = ros::Time();
    marker.ns = ns;

    geometry_msgs::Point start;
    start.x = current_tf.getOrigin().x();
    start.y = current_tf.getOrigin().y();
    start.z = current_tf.getOrigin().z();
    marker.points.push_back(start);

    geometry_msgs::Point end;
    end.x = current_tf.getOrigin().x() + vel.x();
    end.y = current_tf.getOrigin().y() + vel.y();
    end.z = current_tf.getOrigin().z() + vel.z();
    marker.points.push_back(end);

    marker.type = visualization_msgs::Marker::ARROW;
    marker.action = visualization_msgs::Marker::ADD;
    marker.scale.x = 0.015;
    marker.scale.y = 0.025;

    marker.color = utils::get_color_msg(color, 1.0);
    marker.id = marker_id;
    return marker;
}

////Callback for collision checking in ik search//////////////////////
namespace validityFun {
    bool validityCallbackFn(planning_scene::PlanningScenePtr &planning_scene,
                            robot_state::RobotStatePtr kinematic_state,
                            const robot_state::JointModelGroup *joint_model_group,
                            const double *joint_group_variable_values) {
        kinematic_state->setJointGroupPositions(joint_model_group, joint_group_variable_values);
        // Now check for collisions
        collision_detection::CollisionRequest collision_request;
        collision_request.group_name = joint_model_group->getName();
        collision_detection::CollisionResult collision_result;
        // collision_detection::AllowedCollisionMatrix acm = planning_scene->getAllowedCollisionMatrix();
        planning_scene->getCurrentStateNonConst().update();
        planning_scene->checkCollisionUnpadded(collision_request, collision_result, *kinematic_state);
        // planning_scene->checkSelfCollision(collision_request, collision_result, *kinematic_state);

        if (collision_result.collision) {
            // ROS_INFO("IK solution is in collision!");
            return false;
        }
        return true;
    }
}