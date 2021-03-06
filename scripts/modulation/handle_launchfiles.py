import argparse
import time
from subprocess import Popen

"""
IMPORTANT: ENSURE THAT THIS FILE ONLY RELIES ON PYTHON2 COMPATIBLE SYNTAX
"""

gazebo_cmds = {
    'pr2': "roslaunch modulation_rl pr2_empty_world.launch gui:=false".split(" "),
    'tiago': "roslaunch modulation_rl tiago_gazebo.launch robot:=steel tuck_arm:=false laser_model:=false camera_model:=false gui:=false".split(" "),
    'hsr': "roslaunch modulation_rl hsrb_empty_world.launch rviz:=false use_manipulation:=false use_navigation:=false use_perception:=false use_task:=false use_teleop:=false use_web:=false use_laser_odom:=false paused:=false gui:=false".split(" "),
}
moveit_cmds = {
    'pr2': "roslaunch pr2_moveit_config move_group.launch".split(" "),
    'tiago': None,  # ['roslaunch', 'tiago_moveit_config', 'move_group.launch'],
    'hsr': "roslaunch modulation_rl hsr_move_group.launch joint_states_topic:=/hsrb/robot_state/joint_states".split(" ")
}


def start_launch_files(env_name, use_task_world=False):
    gazebo_cmd, moveit_cmd = gazebo_cmds[env_name], moveit_cmds[env_name]

    # fast_empty for fast physics, but not sure node will be able to fully keep up
    world_name = 'modulation_tasks' if use_task_world else 'empty'
    if env_name in ['pr2', 'hsr']:
        world_name += ".world"
    if env_name == 'hsr':
        # other nodes seem unable to handle the fast timestamps for the hsr
        world_name = world_name.replace("fast", "")
    gazebo_cmd += ['world_name:=' + world_name]

    print("Starting command ", gazebo_cmd)
    p_gazebo = Popen(gazebo_cmd) if gazebo_cmd else None
    time.sleep(10)
    print("Starting command ", moveit_cmd)
    p_moveit = Popen(moveit_cmd) if moveit_cmd else None
    time.sleep(30)
    return p_gazebo, p_moveit


def stop_launch_files(p_gazebo, p_moveit):
    time.sleep(10)
    if p_gazebo:
        p_gazebo.terminate()
    if p_moveit:
        p_moveit.terminate()


# def roskill():
#     cmd = 'rosnode kill -a; killall -9 gzserver gzclient'
#     return run(cmd)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--env', type=str.lower,
                        choices=['pr2', 'pr2old', 'tiago', 'hsr', ''], help='')
    parser.add_argument('--use_task_world', action='store_true', default=False, help='')
    args = parser.parse_args()

    if args.env:
        start_launch_files(args.env, use_task_world=args.use_task_world)
        print("\nAll launchfiles started\n")
    else:
        print("No env supplied for startup. Make sure to start directly through the runfile")
