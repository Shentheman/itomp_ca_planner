/*

License

ITOMP Optimization-based Planner
Copyright © and trademark ™ 2014 University of North Carolina at Chapel Hill.
All rights reserved.

Permission to use, copy, modify, and distribute this software and its documentation
for educational, research, and non-profit purposes, without fee, and without a
written agreement is hereby granted, provided that the above copyright notice,
this paragraph, and the following four paragraphs appear in all copies.

This software program and documentation are copyrighted by the University of North
Carolina at Chapel Hill. The software program and documentation are supplied "as is,"
without any accompanying services from the University of North Carolina at Chapel
Hill or the authors. The University of North Carolina at Chapel Hill and the
authors do not warrant that the operation of the program will be uninterrupted
or error-free. The end-user understands that the program was developed for research
purposes and is advised not to rely exclusively on the program for any reason.

IN NO EVENT SHALL THE UNIVERSITY OF NORTH CAROLINA AT CHAPEL HILL OR THE AUTHORS
BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, INCLUDING LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
DOCUMENTATION, EVEN IF THE UNIVERSITY OF NORTH CAROLINA AT CHAPEL HILL OR THE
AUTHORS HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

THE UNIVERSITY OF NORTH CAROLINA AT CHAPEL HILL AND THE AUTHORS SPECIFICALLY
DISCLAIM ANY WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE AND ANY STATUTORY WARRANTY
OF NON-INFRINGEMENT. THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND
THE UNIVERSITY OF NORTH CAROLINA AT CHAPEL HILL AND THE AUTHORS HAVE NO OBLIGATIONS
TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.

Any questions or comments should be sent to the author chpark@cs.unc.edu

*/
#include <itomp_ca_planner/planner/itomp_planner_node.h>
#include <itomp_ca_planner/model/itomp_planning_group.h>
#include <itomp_ca_planner/util/planning_parameters.h>
#include <itomp_ca_planner/visualization/visualization_manager.h>
#include <itomp_ca_planner/precomputation/precomputation.h>
#include <kdl/jntarray.hpp>
#include <angles/angles.h>
#include <visualization_msgs/MarkerArray.h>
#include <boost/random/uniform_real.hpp>
#include <boost/random/variate_generator.hpp>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/robot_model/robot_model.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/robot_state/conversions.h>
#include <Eigen/Geometry>

using namespace std;

namespace itomp_ca_planner
{

ItompPlannerNode::ItompPlannerNode(const robot_model::RobotModelConstPtr& model) :
    last_planning_time_(0), last_min_cost_trajectory_(0), planning_count_(0)
{
	complete_initial_robot_state_.reset(new robot_state::RobotState(model));
}

bool ItompPlannerNode::init()
{
	//Eigen::initParallel();

	PlanningParameters::getInstance()->initFromNodeHandle();

    robot_model_loader::RobotModelLoader robot_model_loader("robot_description");
	robot_model::RobotModelPtr kinematic_model = robot_model_loader.getModel();

	// build the robot model
	string reference_frame = kinematic_model->getModelFrame();
    if (!robot_model_.init(kinematic_model, robot_model_loader.getRobotDescription()))
		return false;

	VisualizationManager::getInstance()->initialize(robot_model_);

    double trajectory_duration = PlanningParameters::getInstance()->getTrajectoryDuration();
    double trajectory_discretization = PlanningParameters::getInstance()->getTrajectoryDiscretization();
	int num_contacts = PlanningParameters::getInstance()->getNumContacts();
    int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();
    trajectory_.reset(new ItompCIOTrajectory(&robot_model_, trajectory_duration,
                      trajectory_discretization, num_contacts,
                      PlanningParameters::getInstance()->getPhaseDuration()));

	resetPlanningInfo(1, 1);

	ROS_INFO("Initialized ITOMP planning service...");

	return true;
}

ItompPlannerNode::~ItompPlannerNode()
{
}

int ItompPlannerNode::run()
{
	return 0;
}

bool ItompPlannerNode::planKinematicPath(const planning_scene::PlanningSceneConstPtr& planning_scene,
        const planning_interface::MotionPlanRequest &req,
        planning_interface::MotionPlanResponse &res)
{
    if (req.planner_id == "ITOMP_3steps")
        return plan3StepPath(planning_scene, req, res);

	// reload parameters
	PlanningParameters::getInstance()->initFromNodeHandle();

	ros::WallTime start_time = ros::WallTime::now();

	//ros::spinOnce();

	if (!preprocessRequest(req))
		return false;

	// generate planning group list
	vector<string> planningGroups;
	getPlanningGroups(planningGroups, req.group_name);

    Precomputation::getInstance()->initialize(planning_scene, robot_model_, req.group_name);


	int num_trials = PlanningParameters::getInstance()->getNumTrials();
	//resetPlanningInfo(num_trials, planningGroups.size());
	for (int c = planning_count_; c < planning_count_ + num_trials; ++c)
	{
		printf("Trial [%d]\n", c);

        Precomputation::getInstance()->createRoadmap();

		// initialize trajectory with start state
        initTrajectory(req.start_state.joint_state, planning_scene);
        complete_initial_robot_state_ = planning_scene->getCurrentStateUpdated(req.start_state);

        Precomputation::getInstance()->addStartState(*complete_initial_robot_state_.get());
		// TODO : addGoalStates

		planning_start_time_ = ros::Time::now().toSec();

        // for each planning group
        for (unsigned int i = 0; i != planningGroups.size(); ++i)
        {
            const string& groupName = planningGroups[i];

            VisualizationManager::getInstance()->setPlanningGroup(robot_model_, groupName);

			// optimize
            if (trajectoryOptimization(groupName, req, planning_scene) == false)
                return false;

            writePlanningInfo(c, i);
        }
	}
    printPlanningInfoSummary();

    // return trajectory
    fillInResult(planningGroups, res);

    planning_count_ += num_trials;

    return true;
}

bool ItompPlannerNode::plan3StepPath(const planning_scene::PlanningSceneConstPtr& planning_scene,
                                     const planning_interface::MotionPlanRequest &req,
                                     planning_interface::MotionPlanResponse &res)
{
    // reload parameters
    PlanningParameters::getInstance()->initFromNodeHandle();

    if (!preprocessRequest(req))
        return false;

    Precomputation::getInstance()->initialize(planning_scene, robot_model_, req.group_name);
    Precomputation::getInstance()->createRoadmap();

    complete_initial_robot_state_ = planning_scene->getCurrentStateUpdated(req.start_state);
    Precomputation::getInstance()->addStartState(*complete_initial_robot_state_.get());

    robot_state::RobotStatePtr start_states[3];
    robot_state::RobotStatePtr goal_states[3];

    for (int i = 0; i < 3; ++i)
    {
        start_states[i].reset(new robot_state::RobotState(*complete_initial_robot_state_));
        goal_states[i].reset(new robot_state::RobotState(*complete_initial_robot_state_));
    }

    if (req.start_state.joint_state.position.size() > 0)
        robot_state::robotStateMsgToRobotState(req.start_state, *start_states[0]);

    if (req.goal_constraints[0].joint_constraints.size() > 0)
    {
        sensor_msgs::JointState joint_goal_state;
        getGoalState(req, joint_goal_state);
        for (int i = 0; i < joint_goal_state.name.size(); ++i)
            goal_states[2]->setVariablePosition(joint_goal_state.name[i], joint_goal_state.position[i]);
    }
    else if (req.goal_constraints[0].orientation_constraints.size() > 0)
    {
        Eigen::Affine3d end_effector_transform;
        const geometry_msgs::Point& req_translation = req.goal_constraints[0].position_constraints[0].constraint_region.primitive_poses[0].position;
        const geometry_msgs::Quaternion& req_orientation = req.goal_constraints[0].orientation_constraints[0].orientation;
        end_effector_transform.translation() = Eigen::Vector3d(req_translation.x, req_translation.y, req_translation.z);
        Eigen::Quaterniond rotation(req_orientation.w, req_orientation.x, req_orientation.y, req_orientation.z);
        end_effector_transform.linear() = rotation.toRotationMatrix();

        const std::string& link_name = req.goal_constraints[0].orientation_constraints[0].link_name;

        if (collisionAwareIK(*goal_states[2], end_effector_transform, req.group_name, link_name, planning_scene) == false)
            return false;
    }

    robot_state::JointModelGroup* jmg = robot_model_.getRobotModel()->getJointModelGroup(req.group_name);
    const std::string ee_name = robot_model_.getGroupEndeffectorLinkName(req.group_name);

    start_states[0]->update(true);
    goal_states[2]->update(true);

    const Eigen::Affine3d& transform_start = start_states[0]->getFrameTransform(ee_name);
    const Eigen::Affine3d& transform_goal = goal_states[2]->getFrameTransform(ee_name);

    Eigen::Affine3d transform_start_shifted = transform_start;
    Eigen::Affine3d transform_goal_shifted = transform_goal;

    const double offset = 0.1;

    // shift by -x
    for (int i = 0; i < 3; ++i)
    {
        transform_start_shifted(i, 3) -= offset * transform_start_shifted(i, 0);
        transform_goal_shifted(i, 3) -= offset * transform_goal_shifted(i, 0);
    }

    kinematics::KinematicsQueryOptions options;
    options.return_approximate_solution = false;

    *goal_states[0] = *start_states[0];
    *start_states[2] = *goal_states[2];
    if (!goal_states[0]->setFromIK(jmg, transform_start_shifted, ee_name, 10, 1.0, moveit::core::GroupStateValidityCallbackFn(), options))
        return false;
    if (!start_states[2]->setFromIK(jmg, transform_goal_shifted, ee_name, 10, 1.0, moveit::core::GroupStateValidityCallbackFn(), options))
        return false;

    goal_states[0]->update(true);
    start_states[2]->update(true);

    *start_states[1] = *goal_states[0];
    *goal_states[1] = *start_states[2];

    planning_start_time_ = ros::Time::now().toSec();
    VisualizationManager::getInstance()->setPlanningGroup(robot_model_, req.group_name);

    std::vector<std::string> planning_groups;
    planning_groups.push_back(req.group_name);

    double total_duration = PlanningParameters::getInstance()->getTrajectoryDuration();
    for (int i = 0; i < 3; ++i)
    {
        double duration = (i == 1) ? total_duration - 1.0 : 0.5;
        PlanningParameters::getInstance()->setTrajectoryDuration(duration);
        initTrajectory(start_states[i], duration);

        trajectoryOptimization(req.group_name, req, planning_scene, goal_states[i]);

        fillInResult(planning_groups, res, i > 0);
    }
    PlanningParameters::getInstance()->setTrajectoryDuration(total_duration);

    return true;
}

bool ItompPlannerNode::preprocessRequest(const planning_interface::MotionPlanRequest &req)
{
	ROS_INFO("Received planning request...");

    ROS_INFO("Trajectory Duration : %f", PlanningParameters::getInstance()->getTrajectoryDuration());

	trajectory_start_time_ = req.start_state.joint_state.header.stamp.toSec();

	// check goal constraint
	ROS_INFO("goal");
	std::vector<sensor_msgs::JointState> goal_joint_states;
	jointConstraintsToJointState(req.goal_constraints, goal_joint_states);
    if (goal_joint_states[0].name.size() != goal_joint_states[0].position.size())
	{
		ROS_ERROR("Invalid goal");
		return false;
	}
	for (unsigned int i = 0; i < goal_joint_states[0].name.size(); i++)
	{
        ROS_INFO("%s %f", goal_joint_states[0].name[i].c_str(), goal_joint_states[0].position[i]);
	}

    ROS_INFO_STREAM("Joint state has " << req.start_state.joint_state.name.size() << " joints");

	return true;
}

void ItompPlannerNode::initTrajectory(const sensor_msgs::JointState &joint_state,
                                      const planning_scene::PlanningSceneConstPtr& planning_scene)
{
    int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();
    double trajectory_duration = PlanningParameters::getInstance()->getTrajectoryDuration();
	if (trajectory_->getDuration() != trajectory_duration)
	{
        double trajectory_discretization = PlanningParameters::getInstance()->getTrajectoryDiscretization();

        trajectory_.reset(new ItompCIOTrajectory(&robot_model_, trajectory_duration,
                          trajectory_discretization,
                          PlanningParameters::getInstance()->getNumContacts(),
                          PlanningParameters::getInstance()->getPhaseDuration()));
	}

	// set the trajectory to initial state value
    start_point_velocities_ = Eigen::MatrixXd(1, robot_model_.getNumKDLJoints());
    start_point_accelerations_ = Eigen::MatrixXd(1, robot_model_.getNumKDLJoints());

    if (joint_state.position.size() != 0)
    {
        robot_model_.jointStateToArray(joint_state, trajectory_->getTrajectoryPoint(0), start_point_velocities_.row(0),
                                       start_point_accelerations_.row(0));
    }
    else
    {
        const robot_state::RobotState& current_state = planning_scene->getCurrentState();
        for (int i = 0; i < current_state.getVariableCount(); ++i)
        {
            (*trajectory_)(0, i) = current_state.getVariablePositions()[i];
            start_point_velocities_(0, i) = current_state.getVariableVelocities()[i];
            start_point_accelerations_(0, i) = current_state.getVariableAccelerations()[i];
        }
    }

	for (int i = 1; i < trajectory_->getNumPoints(); ++i)
	{
		trajectory_->getTrajectoryPoint(i) = trajectory_->getTrajectoryPoint(0);
	}

	// set the contact trajectory initial values
    Eigen::MatrixXd::RowXpr initContacts = trajectory_->getContactTrajectoryPoint(0);
    Eigen::MatrixXd::RowXpr goalContacts = trajectory_->getContactTrajectoryPoint(trajectory_->getNumContactPhases());
	for (int i = 0; i < trajectory_->getNumContacts(); ++i)
	{
        initContacts(i) = PlanningParameters::getInstance()->getContactVariableInitialValues()[i];
        goalContacts(i) = PlanningParameters::getInstance()->getContactVariableGoalValues()[i];
	}
	for (int i = 1; i < trajectory_->getNumContactPhases(); ++i)
	{
		trajectory_->getContactTrajectoryPoint(i) = initContacts;
	}
}

void ItompPlannerNode::initTrajectory(const robot_state::RobotStatePtr& start_state, double duration)
{
    if (trajectory_->getDuration() != duration)
    {
        double trajectory_discretization = PlanningParameters::getInstance()->getTrajectoryDiscretization();

        trajectory_.reset(new ItompCIOTrajectory(&robot_model_, duration,
                          trajectory_discretization,
                          PlanningParameters::getInstance()->getNumContacts(),
                          PlanningParameters::getInstance()->getPhaseDuration()));
    }

    // set the trajectory to initial state value
    start_point_velocities_ = Eigen::MatrixXd(1, robot_model_.getNumKDLJoints());
    start_point_accelerations_ = Eigen::MatrixXd(1, robot_model_.getNumKDLJoints());

    for (int i = 0; i < start_state->getVariableCount(); ++i)
    {
        (*trajectory_)(0, i) = start_state->getVariablePositions()[i];
        start_point_velocities_(0, i) = start_state->getVariableVelocities()[i];
        start_point_accelerations_(0, i) = start_state->getVariableAccelerations()[i];
    }
    for (int i = 1; i < trajectory_->getNumPoints(); ++i)
    {
        trajectory_->getTrajectoryPoint(i) = trajectory_->getTrajectoryPoint(0);
    }
}

void ItompPlannerNode::getGoalState(const planning_interface::MotionPlanRequest &req, sensor_msgs::JointState& goalState)
{
	std::vector<sensor_msgs::JointState> goal_joint_states;
	jointConstraintsToJointState(req.goal_constraints, goal_joint_states);
	goalState.name.resize(req.start_state.joint_state.name.size());
	goalState.position.resize(req.start_state.joint_state.position.size());
	for (unsigned int i = 0; i < goal_joint_states[0].name.size(); ++i)
	{
		string name = goal_joint_states[0].name[i];
		int kdl_number = robot_model_.urdfNameToKdlNumber(name);
		if (kdl_number >= 0)
		{
			goalState.name[kdl_number] = name;
			goalState.position[kdl_number] = goal_joint_states[0].position[i];
		}
	}

    std::vector<robot_state::RobotState> robot_states(goal_joint_states.size(), *complete_initial_robot_state_);
	for (int i = 0; i < goal_joint_states.size(); ++i)
	{
        robot_state::jointStateToRobotState(goal_joint_states[i], robot_states[i]);
	}

    Precomputation::getInstance()->addGoalStates(robot_states);
}

void ItompPlannerNode::getPlanningGroups(std::vector<std::string>& plannningGroups, const string& groupName)
{
	plannningGroups.clear();
	if (groupName == "decomposed_body")
	{
		plannningGroups.push_back("lower_body");
		plannningGroups.push_back("torso");
		//plannningGroups.push_back("head");
		plannningGroups.push_back("left_arm");
		plannningGroups.push_back("right_arm");
	}
	else
	{
		plannningGroups.push_back(groupName);
	}
}

void optimization_thread_function(ItompOptimizerPtr& optimizer)
{
	omp_set_num_threads(getNumParallelThreads());
	optimizer->optimize();
}

bool ItompPlannerNode::trajectoryOptimization(const string& groupName,
        const planning_interface::MotionPlanRequest &req,
        const planning_scene::PlanningSceneConstPtr& planning_scene)
{
    if (fillGroupJointTrajectory(groupName, req, planning_scene) == false)
        return false;

    ros::WallTime create_time = ros::WallTime::now();

    int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();
	const ItompPlanningGroup* group = robot_model_.getPlanningGroup(groupName);

	best_cost_manager_.reset();

	optimizers_.resize(num_trajectories);
	for (int i = 0; i < num_trajectories; ++i)
        optimizers_[i].reset(new ItompOptimizer(i, trajectories_[i].get(), &robot_model_,
                                                group, planning_start_time_, trajectory_start_time_,
                                                req.path_constraints, &best_cost_manager_, planning_scene));

    std::vector<boost::shared_ptr<boost::thread> > optimization_threads(num_trajectories);
	for (int i = 0; i < num_trajectories; ++i)
        optimization_threads[i].reset(new boost::thread(optimization_thread_function, optimizers_[i]));

	for (int i = 0; i < num_trajectories; ++i)
		optimization_threads[i]->join();

	last_planning_time_ = (ros::WallTime::now() - create_time).toSec();
    ROS_INFO("Optimization of group %s took %f sec", groupName.c_str(), last_planning_time_);

    return true;
}

void ItompPlannerNode::trajectoryOptimization(const std::string& groupName,
        const planning_interface::MotionPlanRequest& req,
        const planning_scene::PlanningSceneConstPtr& planning_scene,
        const robot_state::RobotStatePtr& goal_state)
{
    fillGroupJointTrajectory(groupName, goal_state);

    ros::WallTime create_time = ros::WallTime::now();

    int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();
    const ItompPlanningGroup* group = robot_model_.getPlanningGroup(groupName);

    best_cost_manager_.reset();

    optimizers_.resize(num_trajectories);
    for (int i = 0; i < num_trajectories; ++i)
        optimizers_[i].reset(new ItompOptimizer(i, trajectories_[i].get(), &robot_model_,
                                                group, planning_start_time_, trajectory_start_time_,
                                                req.path_constraints, &best_cost_manager_, planning_scene));

    std::vector<boost::shared_ptr<boost::thread> > optimization_threads(num_trajectories);
    for (int i = 0; i < num_trajectories; ++i)
        optimization_threads[i].reset(new boost::thread(optimization_thread_function, optimizers_[i]));

    for (int i = 0; i < num_trajectories; ++i)
        optimization_threads[i]->join();

    last_planning_time_ = (ros::WallTime::now() - create_time).toSec();
    ROS_INFO("Optimization of group %s took %f sec", groupName.c_str(), last_planning_time_);
}

void ItompPlannerNode::fillInResult(const std::vector<std::string>& planningGroups,
                                    planning_interface::MotionPlanResponse &res,
                                    bool append)
{
	int best_trajectory_index = best_cost_manager_.getBestCostTrajectoryIndex();

    const std::map<std::string, double>& joint_velocity_limits = PlanningParameters::getInstance()->getJointVelocityLimits();

	int num_all_joints = complete_initial_robot_state_->getVariableCount();

    if (!append)
    {
        res.trajectory_.reset(new robot_trajectory::RobotTrajectory(robot_model_.getRobotModel(), ""));
        res.trajectory_->setGroupName(planningGroups[0]);
    }

    std::vector<double> velocity_limits(num_all_joints, std::numeric_limits<double>::max());

	robot_state::RobotState ks = *complete_initial_robot_state_;
	std::vector<double> positions(num_all_joints);
	double duration = trajectories_[best_trajectory_index]->getDiscretization();
    for (std::size_t i = 0; i < trajectories_[best_trajectory_index]->getNumPoints(); ++i)
	{
		for (std::size_t j = 0; j < num_all_joints; j++)
		{
			positions[j] = (*trajectories_[best_trajectory_index])(i, j);
		}

		ks.setVariablePositions(&positions[0]);
		// TODO: copy vel/acc
		ks.update();

		res.trajectory_->addSuffixWayPoint(ks, duration);
	}
	res.error_code_.val = moveit_msgs::MoveItErrorCodes::SUCCESS;

	// print results
    if (PlanningParameters::getInstance()->getPrintPlanningInfo())
    {
        const std::vector<std::string>& joint_names = res.trajectory_->getFirstWayPoint().getVariableNames();
        for (int j = 0; j < num_all_joints; j++)
            printf("%s ", joint_names[j].c_str());
        printf("\n");
        for (int i = 0; i < trajectories_[best_trajectory_index]->getNumPoints(); ++i)
        {
            for (int j = 0; j < num_all_joints; j++)
            {
                printf("%f ", res.trajectory_->getWayPoint(i).getVariablePosition(j));
            }
            printf("\n");
        }
    }

}

bool ItompPlannerNode::fillGroupJointTrajectory(const string& group_name,
        const planning_interface::MotionPlanRequest &req,
        const planning_scene::PlanningSceneConstPtr& planning_scene)
{
    int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();

    const ItompPlanningGroup* group = robot_model_.getPlanningGroup(group_name);

    if (req.goal_constraints[0].joint_constraints.size() > 0)
    {
        sensor_msgs::JointState jointGoalState;
        getGoalState(req, jointGoalState);

        int goal_index = trajectory_->getNumPoints() - 1;
        Eigen::MatrixXd::RowXpr goalPoint = trajectory_->getTrajectoryPoint(goal_index);
        for (int i = 0; i < group->num_joints_; ++i)
        {
            string name = group->group_joints_[i].joint_name_;
            int kdl_number = robot_model_.urdfNameToKdlNumber(name);
            if (kdl_number >= 0)
            {
                goalPoint(kdl_number) = jointGoalState.position[kdl_number];
            }
        }
    }
    else if (req.goal_constraints[0].orientation_constraints.size() > 0)
    {
        std::vector<robot_state::RobotState> robot_states;
        robot_state::RobotState robot_state(*complete_initial_robot_state_);

        Eigen::Affine3d end_effector_transform;
        const geometry_msgs::Point& req_translation = req.goal_constraints[0].position_constraints[0].constraint_region.primitive_poses[0].position;
        const geometry_msgs::Quaternion& req_orientation = req.goal_constraints[0].orientation_constraints[0].orientation;
        end_effector_transform.translation() = Eigen::Vector3d(req_translation.x, req_translation.y, req_translation.z);
        Eigen::Quaterniond rotation(req_orientation.w, req_orientation.x, req_orientation.y, req_orientation.z);
        end_effector_transform.linear() = rotation.toRotationMatrix();

        const std::string& link_name = req.goal_constraints[0].orientation_constraints[0].link_name;

        if (collisionAwareIK(robot_state, end_effector_transform, group_name, link_name, planning_scene))
        {
            int goal_index = trajectory_->getNumPoints() - 1;
            Eigen::MatrixXd::RowXpr goalPoint = trajectory_->getTrajectoryPoint(goal_index);
            for (int i = 0; i < group->num_joints_; ++i)
            {
                string name = group->group_joints_[i].joint_name_;
                int kdl_number = robot_model_.urdfNameToKdlNumber(name);
                if (kdl_number >= 0)
                {
                    goalPoint(kdl_number) = robot_state.getVariablePosition(name);
                }
            }
        }
        else
            return false;

        /*
        for (int i = 0; i < 100; ++i)
        {
            if (collisionAwareIK(robot_state, end_effector_transform, group_name, link_name, planning_scene))
            {
                robot_states.push_back(robot_state);

                const double DELTA = 0.04;
                Eigen::VectorXd d(robot_state.getVariableCount());
                for (int j = 0; j < d.size(); ++j)
                    d(j) = DELTA * 1.0 / std::sqrt(robot_state.getVariableCount());

                robot_state::RobotState new_state(robot_state);
                for (int j = 0; j < robot_state.getVariableCount(); ++j)
                    new_state.getVariablePositions()[j] = robot_state.getVariablePositions()[j] + d(j);
                new_state.update(true);

                const Eigen::Affine3d& test_transform = robot_state.getGlobalLinkTransform(link_name);
                Eigen::Vector3d test_ea = test_transform.linear().eulerAngles(0, 1, 2);

                const Eigen::Affine3d& new_transform = new_state.getGlobalLinkTransform(link_name);
                Eigen::Vector3d ea = new_transform.linear().eulerAngles(0, 1, 2);

                // ts
                Eigen::MatrixXd jacobian;
                const robot_state::JointModelGroup* joint_model_group = robot_model_.getRobotModel()->getJointModelGroup(group_name);
                robot_state.getJacobian(joint_model_group, new_state.getLinkModel(link_name), Eigen::Vector3d(0, 0, 0), jacobian);

                Eigen::MatrixXd jacobian_pseudo_inverse = pseudoInverse(jacobian);
                Eigen::MatrixXd c = Eigen::MatrixXd::Zero(6, 6);
                c(3,3) = 1.0;
                c(4,4) = 1.0;
                Eigen::VectorXd new_d(robot_state.getVariableCount());

                new_d = (Eigen::MatrixXd::Identity(7,7) - jacobian_pseudo_inverse * c * jacobian) * d;


                // fr
                Eigen::MatrixXd jacobian2;
                new_state.getJacobian(joint_model_group, new_state.getLinkModel(link_name), Eigen::Vector3d(0, 0, 0), jacobian2);
                Eigen::MatrixXd jacobian_pseudo_inverse2 = pseudoInverse(jacobian2);
                Eigen::VectorXd new_d2(robot_state.getVariableCount());
                Eigen::VectorXd ea6 = Eigen::VectorXd::Zero(6);
                ea6(3) = ea(0);
                ea6(4) = ea(1);
                ea6(5) = ea(2);
                new_d2 = -jacobian_pseudo_inverse2 * ea6;

                // test
                for (int j = 0; j < robot_state.getVariableCount(); ++j)
                    new_state.getVariablePositions()[j] = robot_state.getVariablePositions()[j] + new_d(j);
                new_state.update(true);
                const Eigen::Affine3d& new_transform2 = new_state.getGlobalLinkTransform(link_name);
                Eigen::Vector3d ea2 = new_transform2.linear().eulerAngles(0, 1, 2);

                for (int j = 0; j < robot_state.getVariableCount(); ++j)
                    new_state.getVariablePositions()[j] = robot_state.getVariablePositions()[j] + new_d2(j);
                new_state.update(true);
                const Eigen::Affine3d& new_transform3 = new_state.getGlobalLinkTransform(link_name);
                Eigen::Vector3d ea3 = new_transform3.linear().eulerAngles(0, 1, 2);
            }
            robot_state.setToRandomPositions();
        }

        Precomputation::getInstance()->addGoalStates(robot_states);
        */
    }

    moveit_msgs::TrajectoryConstraints precomputation_trajectory_constraints;
    Precomputation::getInstance()->extractInitialTrajectories(precomputation_trajectory_constraints);

	std::set<int> groupJointsKDLIndices;
	for (int i = 0; i < group->num_joints_; ++i)
	{
		groupJointsKDLIndices.insert(group->group_joints_[i].kdl_joint_index_);
	}

	trajectories_.resize(num_trajectories);
	for (int i = 0; i < num_trajectories; ++i)
	{
        trajectories_[i].reset(new ItompCIOTrajectory(&robot_model_,
                               trajectory_->getDuration(),
                               trajectory_->getDiscretization(),
                               PlanningParameters::getInstance()->getNumContacts(),
                               PlanningParameters::getInstance()->getPhaseDuration()));

		*(trajectories_[i].get()) = *(trajectory_.get());

		if (/*i != 0 && */precomputation_trajectory_constraints.constraints.size() != 0)
		{
			trajectories_[i]->fillInMinJerk(i, groupJointsKDLIndices, group,
                                            precomputation_trajectory_constraints, start_point_velocities_.row(0),
                                            start_point_accelerations_.row(0));
		}
        else if (req.path_constraints.position_constraints.size() != 0)
		{
            trajectories_[i]->fillInMinJerkCartesianTrajectory(groupJointsKDLIndices, start_point_velocities_.row(0),
                    start_point_accelerations_.row(0), req.path_constraints, group_name);
		}
		else
		{
            trajectories_[i]->fillInMinJerk(groupJointsKDLIndices,
                                            start_point_velocities_.row(0),
                                            start_point_accelerations_.row(0));
		}
	}

    return true;
}


void ItompPlannerNode::fillGroupJointTrajectory(const string& group_name,
        const robot_state::RobotStatePtr& goal_state)
{
    int num_trajectories = PlanningParameters::getInstance()->getNumTrajectories();

    const ItompPlanningGroup* group = robot_model_.getPlanningGroup(group_name);
    int goal_index = trajectory_->getNumPoints() - 1;
    Eigen::MatrixXd::RowXpr goalPoint = trajectory_->getTrajectoryPoint(goal_index);
    for (int i = 0; i < group->num_joints_; ++i)
    {
        string name = group->group_joints_[i].joint_name_;
        int kdl_number = robot_model_.urdfNameToKdlNumber(name);
        if (kdl_number >= 0)
        {
            goalPoint(kdl_number) = goal_state->getVariablePositions()[kdl_number];
        }
    }

    std::set<int> groupJointsKDLIndices;
    for (int i = 0; i < group->num_joints_; ++i)
    {
        groupJointsKDLIndices.insert(group->group_joints_[i].kdl_joint_index_);
    }

    trajectories_.resize(num_trajectories);
    for (int i = 0; i < num_trajectories; ++i)
    {
        trajectories_[i].reset(new ItompCIOTrajectory(&robot_model_,
                               trajectory_->getDuration(),
                               trajectory_->getDiscretization(),
                               PlanningParameters::getInstance()->getNumContacts(),
                               PlanningParameters::getInstance()->getPhaseDuration()));

        *(trajectories_[i].get()) = *(trajectory_.get());
        trajectories_[i]->fillInMinJerk(groupJointsKDLIndices,
                                        start_point_velocities_.row(0),
                                        start_point_accelerations_.row(0));
    }
}

void ItompPlannerNode::resetPlanningInfo(int trials, int component)
{
	planning_info_.clear();
	planning_info_.resize(trials, std::vector<PlanningInfo>(component));
}

void ItompPlannerNode::writePlanningInfo(int trials, int component)
{
	int best_trajectory_index = best_cost_manager_.getBestCostTrajectoryIndex();

	if (planning_info_.size() <= trials)
        planning_info_.resize(trials + 1, std::vector<PlanningInfo>(planning_info_[0].size()));
	PlanningInfo& info = planning_info_[trials][component];
	info.time = last_planning_time_;
    info.iterations = optimizers_[best_trajectory_index]->getLastIteration() + 1;
	info.cost = optimizers_[best_trajectory_index]->getBestCost();
	info.success = (optimizers_[best_trajectory_index]->isSucceed() ? 1 : 0);
}

void ItompPlannerNode::printPlanningInfoSummary()
{
	int numPlannings = planning_info_.size();
	int numComponents = planning_info_[0].size();

	vector<PlanningInfo> summary(numComponents);
	PlanningInfo sumOfSum;
	for (int j = 0; j < numComponents; ++j)
	{
		for (int i = 0; i < numPlannings; ++i)
		{
			summary[j] += planning_info_[i][j];
		}
		sumOfSum += summary[j];
	}

	// compute success rate
	// if a component fails, that trail fails.
	int numSuccess = 0;
	for (int i = 0; i < numPlannings; ++i)
	{
		bool failed = false;
		for (int j = 0; j < numComponents; ++j)
		{
			if (planning_info_[i][j].success == 0)
			{
				failed = true;
				break;
			}
		}
		if (!failed)
		{
			++numSuccess;
		}
	}

	printf("%d Trials, %d components\n", numPlannings, numComponents);
	printf("Component Iterations Time Smoothness SuccessRate\n");
	for (int j = 0; j < numComponents; ++j)
	{
		printf("%d %f %f %f %f\n", j,
               ((double) summary[j].iterations) / numPlannings,
               ((double) summary[j].time) / numPlannings,
               ((double) summary[j].cost) / numPlannings,
               ((double) summary[j].success) / numPlannings);
	}
	printf("Sum %f %f %f %f\n", ((double) sumOfSum.iterations) / numPlannings,
           ((double) sumOfSum.time) / numPlannings,
           ((double) sumOfSum.cost) / numPlannings,
           ((double) numSuccess) / numPlannings);
	printf("\n");

	printf("plannings info\n");
	printf("Component Iterations Time Smoothness SuccessRate\n");
	for (int i = 0; i < numPlannings; ++i)
	{
		double iterationsSum = 0, timeSum = 0, costSum = 0;
		for (int j = 0; j < numComponents; ++j)
		{
			iterationsSum += planning_info_[i][j].iterations;
			timeSum += planning_info_[i][j].time;
			costSum += planning_info_[i][j].cost;
		}
		printf("[%d] %f %f %f \n", i, iterationsSum, timeSum, costSum);

	}
}

bool ItompPlannerNode::collisionAwareIK(robot_state::RobotState& robot_state, const Eigen::Affine3d& transform,
                                        const std::string& group_name, const std::string& link_name,
                                        const planning_scene::PlanningSceneConstPtr& planning_scene) const
{
    const robot_state::JointModelGroup* joint_model_group = robot_model_.getRobotModel()->getJointModelGroup(group_name);
    kinematics::KinematicsQueryOptions options;
    options.return_approximate_solution = false;

    bool found_ik = false;

    int i = 0;
    int max_trial = 100;
    while (i++ <= max_trial)
    {
        found_ik = robot_state.setFromIK(joint_model_group, transform, link_name, 10,
                                         1.0, moveit::core::GroupStateValidityCallbackFn(), options);
        if (!found_ik)
            break;

        robot_state.update();
        found_ik = planning_scene->isStateValid(robot_state, "", i == max_trial);
        if (found_ik)
        {
            break;
        }

        robot_state.setToRandomPositions();
    }
    if (!found_ik)
    {
        ROS_ERROR("Could not find IK solution");
    }

    return found_ik;
}

} // namespace
