#include "core/commands/IntCommand.hpp"
#include "core/commands/ListCommand.hpp"
#include "core/commands/LoopedCommand.hpp"
#include "core/frontend/Notifications.hpp"
#include "game/backend/Self.hpp"
#include "game/gta/Natives.hpp"
#include "types/pad/ControllerInputs.hpp"

namespace YimMenu::Features
{
	static constexpr int lawful_driving_style = 786603;
	static constexpr int ignore_lights_driving_style = 2883621;
	static constexpr int aggressive_driving_style = 1074528293;

	static IntCommand _AutoDriveSpeed{
	    "autodrivespeed",
	    "Cruise Speed (km/h)",
	    "The target speed used by Auto Drive",
	    20,
	    160,
	    70};

	static std::vector<std::pair<int, const char*>> g_AutoDriveStyles = {
	    {lawful_driving_style, "Lawful"},
	    {ignore_lights_driving_style, "Ignore Traffic Lights"},
	    {aggressive_driving_style, "Aggressive"}};

	static ListCommand _AutoDriveStyle{
	    "autodrivestyle",
	    "Driving Style",
	    "How Auto Drive behaves around traffic and traffic lights",
	    g_AutoDriveStyles,
	    lawful_driving_style};

	class AutoDrive : public LoopedCommand
	{
		using LoopedCommand::LoopedCommand;

		enum class Mode
		{
			Idle,
			Waypoint,
			Wander,
			Arrived
		};

		enum class FailureReason
		{
			None,
			NoVehicle,
			PlayerDead,
			NotDriver,
			UnsupportedVehicle,
			VehicleUndriveable,
			NoControl
		};

		static constexpr float manual_input_deadzone = 0.2f;
		static constexpr float waypoint_move_threshold_squared = 25.0f;
		static constexpr float arrival_distance_squared = 100.0f;
		static constexpr float stopping_range = 8.0f;

		Mode m_Mode = Mode::Idle;
		FailureReason m_FailureReason = FailureReason::None;
		int m_DriverHandle = 0;
		int m_VehicleHandle = 0;
		int m_LastSpeedKph = -1;
		int m_LastDrivingStyle = -1;
		Vector3 m_Waypoint{};
		Vector3 m_RoadTarget{};
		bool m_HasTask = false;

		static float DistanceSquared2D(const Vector3& first, const Vector3& second)
		{
			const auto deltaX = first.x - second.x;
			const auto deltaY = first.y - second.y;
			return deltaX * deltaX + deltaY * deltaY;
		}

		static float DistanceSquared2D(const rage::fvector3& first, const Vector3& second)
		{
			const auto deltaX = first.x - second.x;
			const auto deltaY = first.y - second.y;
			return deltaX * deltaX + deltaY * deltaY;
		}

		static float GetCruiseSpeed()
		{
			return _AutoDriveSpeed.GetState() / 3.6f;
		}

		static bool IsSupportedRoadVehicle(Vehicle vehicle)
		{
			const auto model = ENTITY::GET_ENTITY_MODEL(vehicle.GetHandle());
			return VEHICLE::IS_THIS_MODEL_A_CAR(model)
			    || VEHICLE::IS_THIS_MODEL_A_BIKE(model)
			    || VEHICLE::IS_THIS_MODEL_A_BICYCLE(model)
			    || VEHICLE::IS_THIS_MODEL_A_QUADBIKE(model);
		}

		static bool HasManualDrivingInput()
		{
			const auto steering = std::abs(PAD::GET_CONTROL_NORMAL(0, static_cast<int>(ControllerInputs::INPUT_VEH_MOVE_LR)));
			const auto accelerate = std::abs(PAD::GET_CONTROL_NORMAL(0, static_cast<int>(ControllerInputs::INPUT_VEH_ACCELERATE)));
			const auto brake = std::abs(PAD::GET_CONTROL_NORMAL(0, static_cast<int>(ControllerInputs::INPUT_VEH_BRAKE)));

			return steering > manual_input_deadzone
			    || accelerate > manual_input_deadzone
			    || brake > manual_input_deadzone
			    || PAD::IS_CONTROL_PRESSED(0, static_cast<int>(ControllerInputs::INPUT_VEH_HANDBRAKE))
			    || PAD::IS_CONTROL_PRESSED(0, static_cast<int>(ControllerInputs::INPUT_VEH_EXIT));
		}

		static bool TryGetWaypoint(Vector3& waypoint)
		{
			if (!HUD::IS_WAYPOINT_ACTIVE())
				return false;

			const auto blip = HUD::GET_CLOSEST_BLIP_INFO_ID(HUD::GET_WAYPOINT_BLIP_ENUM_ID());
			if (!HUD::DOES_BLIP_EXIST(blip))
				return false;

			waypoint = HUD::GET_BLIP_COORDS(blip);
			return true;
		}

		static Vector3 ResolveRoadTarget(const Vector3& waypoint)
		{
			Vector3 roadTarget = waypoint;
			float heading = 0.0f;
			if (!PATH::GET_CLOSEST_VEHICLE_NODE_WITH_HEADING(
			        waypoint.x,
			        waypoint.y,
			        waypoint.z,
			        &roadTarget,
			        &heading,
			        1,
			        3.0f,
			        0.0f))
				return waypoint;

			return roadTarget;
		}

		void SetFailure(FailureReason reason, std::string_view message = {})
		{
			if (reason == m_FailureReason)
				return;

			m_FailureReason = reason;
			if (reason != FailureReason::None)
				Notifications::Show("Auto Drive", std::string(message), NotificationType::Warning);
		}

		void ResetState()
		{
			m_Mode = Mode::Idle;
			m_DriverHandle = 0;
			m_VehicleHandle = 0;
			m_LastSpeedKph = -1;
			m_LastDrivingStyle = -1;
			m_Waypoint = {};
			m_RoadTarget = {};
			m_HasTask = false;
		}

		void ClearTask()
		{
			if (!m_HasTask)
			{
				ResetState();
				return;
			}

			if (m_DriverHandle && ENTITY::DOES_ENTITY_EXIST(m_DriverHandle))
			{
				TASK::CLEAR_PED_TASKS(m_DriverHandle);
				PED::SET_PED_KEEP_TASK(m_DriverHandle, false);
			}

			if (m_VehicleHandle && ENTITY::DOES_ENTITY_EXIST(m_VehicleHandle))
				TASK::CLEAR_PRIMARY_VEHICLE_TASK(m_VehicleHandle);

			ResetState();
		}

		void AssignVehicle(Ped driver, Vehicle vehicle)
		{
			m_DriverHandle = driver.GetHandle();
			m_VehicleHandle = vehicle.GetHandle();
		}

		void StartWaypointTask(Ped driver, Vehicle vehicle, const Vector3& waypoint)
		{
			AssignVehicle(driver, vehicle);
			m_Waypoint = waypoint;
			m_RoadTarget = ResolveRoadTarget(waypoint);
			m_LastSpeedKph = _AutoDriveSpeed.GetState();
			m_LastDrivingStyle = _AutoDriveStyle.GetState();

			PED::SET_PED_KEEP_TASK(m_DriverHandle, true);
			TASK::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(
			    m_DriverHandle,
			    m_VehicleHandle,
			    m_RoadTarget.x,
			    m_RoadTarget.y,
			    m_RoadTarget.z,
			    GetCruiseSpeed(),
			    m_LastDrivingStyle,
			    stopping_range);

			m_Mode = Mode::Waypoint;
			m_HasTask = true;
		}

		void StartWanderTask(Ped driver, Vehicle vehicle)
		{
			AssignVehicle(driver, vehicle);
			m_LastSpeedKph = _AutoDriveSpeed.GetState();
			m_LastDrivingStyle = _AutoDriveStyle.GetState();

			PED::SET_PED_KEEP_TASK(m_DriverHandle, true);
			TASK::TASK_VEHICLE_DRIVE_WANDER(
			    m_DriverHandle,
			    m_VehicleHandle,
			    GetCruiseSpeed(),
			    m_LastDrivingStyle);

			m_Mode = Mode::Wander;
			m_HasTask = true;
		}

		bool ValidateDriver(Ped driver, Vehicle vehicle)
		{
			if (!driver || !vehicle)
			{
				ClearTask();
				SetFailure(FailureReason::NoVehicle, "Enter the driver seat of a supported road vehicle.");
				return false;
			}

			if (driver.IsDead())
			{
				ClearTask();
				SetFailure(FailureReason::PlayerDead, "Auto Drive is waiting for the player to respawn.");
				return false;
			}

			if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle.GetHandle(), -1, false) != driver.GetHandle())
			{
				ClearTask();
				SetFailure(FailureReason::NotDriver, "Move to the driver seat to use Auto Drive.");
				return false;
			}

			if (!IsSupportedRoadVehicle(vehicle))
			{
				ClearTask();
				SetFailure(FailureReason::UnsupportedVehicle, "This vehicle type does not support Auto Drive.");
				return false;
			}

			if (!VEHICLE::IS_VEHICLE_DRIVEABLE(vehicle.GetHandle(), false))
			{
				ClearTask();
				SetFailure(FailureReason::VehicleUndriveable, "The current vehicle cannot be driven.");
				return false;
			}

			if (!vehicle.RequestControl(0))
			{
				ClearTask();
				SetFailure(FailureReason::NoControl, "Waiting for network control of the current vehicle.");
				return false;
			}

			SetFailure(FailureReason::None);
			return true;
		}

		virtual void OnTick() override
		{
			auto driver = Self::GetPed();
			auto vehicle = Self::GetVehicle();

			if (!ValidateDriver(driver, vehicle))
				return;

			if (m_HasTask && (m_DriverHandle != driver.GetHandle() || m_VehicleHandle != vehicle.GetHandle()))
				ClearTask();

			if (HasManualDrivingInput())
			{
				ClearTask();
				SetState(false);
				return;
			}

			const auto speedChanged = m_LastSpeedKph != _AutoDriveSpeed.GetState();
			const auto styleChanged = m_LastDrivingStyle != _AutoDriveStyle.GetState();

			Vector3 waypoint;
			if (TryGetWaypoint(waypoint))
			{
				const auto waypointChanged = m_Mode == Mode::Idle
				    || m_Mode == Mode::Wander
				    || DistanceSquared2D(m_Waypoint, waypoint) > waypoint_move_threshold_squared;

				if (waypointChanged || (styleChanged && m_Mode == Mode::Waypoint))
				{
					StartWaypointTask(driver, vehicle, waypoint);
					return;
				}

				if (speedChanged && m_HasTask)
				{
					TASK::SET_DRIVE_TASK_CRUISE_SPEED(m_DriverHandle, GetCruiseSpeed());
					m_LastSpeedKph = _AutoDriveSpeed.GetState();
				}

				if (m_Mode == Mode::Waypoint && DistanceSquared2D(vehicle.GetPosition(), m_RoadTarget) <= arrival_distance_squared)
					m_Mode = Mode::Arrived;

				if (m_Mode == Mode::Arrived)
					m_LastDrivingStyle = _AutoDriveStyle.GetState();

				return;
			}

			if (m_Mode != Mode::Wander || styleChanged)
			{
				StartWanderTask(driver, vehicle);
				return;
			}

			if (speedChanged)
			{
				TASK::SET_DRIVE_TASK_CRUISE_SPEED(m_DriverHandle, GetCruiseSpeed());
				m_LastSpeedKph = _AutoDriveSpeed.GetState();
			}
		}

		virtual void OnDisable() override
		{
			ClearTask();
			m_FailureReason = FailureReason::None;
		}
	};

	static AutoDrive _AutoDrive{
	    "autodrive",
	    "Auto Drive",
	    "Drives to your waypoint using roads, or roams when no waypoint is set"};
}