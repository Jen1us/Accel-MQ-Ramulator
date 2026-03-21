#ifndef ENGINE_H
#define ENGINE_H

#include <iostream>
#include <unordered_map>
#include "Sim_Defs.h"
#include "EventTree.h"
#include "Sim_Object.h"

namespace MQSimEngine {
	class Engine
	{
		friend class EventTree;
	public:
		Engine()
		{
			this->_EventList = new EventTree;
			_sim_time = 0;
			stop = false;
			started = false;
			integrated_execution_mode = false;
		}

		~Engine() {
			delete _EventList;
		}
		
		static Engine* Instance();
		sim_time_type Time();
		Sim_Event* Register_sim_event(sim_time_type fireTime, Sim_Object* targetObject, void* parameters = NULL, int type = 0);
		void Ignore_sim_event(Sim_Event*);
		void Reset();
		void AddObject(Sim_Object* obj);
		Sim_Object* GetObject(sim_object_id_type object_id);
		void RemoveObject(Sim_Object* obj);
		// Initializes the simulation objects (trigger hookup, validation, start)
		// without running the event loop. This is useful for co-simulation where
		// the caller advances time in small increments.
		void Initialize_simulation();
		// Process simulation events up to (and including) until_time, then advance
		// the simulator time to until_time. No events after until_time are
		// processed, and the event queue is preserved for subsequent calls.
		void Run_until(sim_time_type until_time);
		void Start_simulation();
		void Stop_simulation();
		bool Has_started();
		bool Is_integrated_execution_mode();
		void Set_integrated_execution_mode(bool enabled) { integrated_execution_mode = enabled; }
	private:
		sim_time_type _sim_time;
		EventTree* _EventList;
		std::unordered_map<sim_object_id_type, Sim_Object*> _ObjectList;
		bool stop;
		bool started;
		bool integrated_execution_mode;
		static Engine* _instance;
	};
}

#define Simulator MQSimEngine::Engine::Instance()
#endif // !ENGINE_H
