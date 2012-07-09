#ifndef _RIPOSTE_INTERPRETER_H
#define _RIPOSTE_INTERPRETER_H

#include <map>
#include <set>
#include <deque>

#include "value.h"
#include "thread.h"
#include "ir.h"

#define USE_LLVM_COMPILER

class Thread;

////////////////////////////////////////////////////////////////////
// VM ops 
///////////////////////////////////////////////////////////////////

#define DECLARE_INTERPRETER_FNS(bc,name,...) \
		Instruction const * bc##_op(Thread& state, Instruction const& inst);

BYTECODES(DECLARE_INTERPRETER_FNS)

////////////////////////////////////////////////////////////////////
// VM data structures
///////////////////////////////////////////////////////////////////


// TODO: Make this use a good concurrent map implementation 
class StringTable {
	std::map<std::string, String> stringTable;
	Lock lock;
public:
	StringTable() {
	#define ENUM_STRING_TABLE(name, string) \
		stringTable[string] = Strings::name; 
		STRINGS(ENUM_STRING_TABLE);
	}

	String in(std::string const& s) {
		lock.acquire();
		std::map<std::string, String>::const_iterator i = stringTable.find(s);
		if(i == stringTable.end()) {
			char* str = new char[s.size()+1];
			memcpy(str, s.c_str(), s.size()+1);
			String string = (String)str;
			stringTable[s] = string;
			lock.release();
			return string;
		} else {
			lock.release();
			return i->second;
		}
	}

	std::string out(String s) const {
		return std::string(s);
	}
};


struct CompiledCall : public gc {
	List call;

	PairList arguments;
	int64_t dotIndex;
	bool named;
	
	explicit CompiledCall(List const& call, PairList arguments, int64_t dotIndex, bool named) 
		: call(call), arguments(arguments), dotIndex(dotIndex), named(named) {}
};

struct Prototype : public gc {
	Value expression;
	String string;

	PairList parameters;
	int dotIndex;

	int registers;
	std::vector<Value, traceable_allocator<Value> > constants;
	std::vector<CompiledCall, traceable_allocator<CompiledCall> > calls; 

	std::vector<Instruction> bc;			// bytecode
	mutable std::vector<Instruction> tbc;		// threaded bytecode
};

struct StackFrame {
	Environment* environment;
	Prototype const* prototype;

	Instruction const* returnpc;
	Value* returnbase;
	
	int64_t dest;
	Environment* env;
};

// TODO: Careful, args and result might overlap!
typedef void (*InternalFunctionPtr)(Thread& s, Value const* args, Value& result);

struct InternalFunction {
	InternalFunctionPtr ptr;
	int64_t params;
};

#ifdef ENABLE_JIT

#define TRACE_MAX_VECTOR_REGISTERS (32)
#define TRACE_VECTOR_WIDTH (64)
//maximum number of instructions to record before dropping out of the
//recording interpreter
#define TRACE_MAX_RECORDED (1024)

struct TraceCodeBuffer;
class Trace : public gc {

	public:	

		std::vector<IRNode, traceable_allocator<IRNode> > nodes;
		std::set<Environment*> liveEnvironments;

		struct Output {
			enum Type { REG, MEMORY };
			Type type;
			union {
				Value* reg;
				Environment::Pointer pointer;
			};
			IRef ref;	   //location of the associated store
		};

		std::vector<Output> outputs;

		TraceCodeBuffer * code_buffer;

		size_t n_recorded_since_last_exec;

		int64_t Size;

		Trace();

		IRef EmitCoerce(IRef a, Type::Enum dst_type);
		IRef EmitUnary(IROpCode::Enum op, Type::Enum type, IRef a, int64_t data); 
		IRef EmitBinary(IROpCode::Enum op, Type::Enum type, IRef a, IRef b, int64_t data);
		IRef EmitTrinary(IROpCode::Enum op, Type::Enum type, IRef a, IRef b, IRef c);
		IRef EmitIfElse(IRef a, IRef b, IRef cond);

		IRef EmitFilter(IRef a, IRef b);
		IRef EmitSplit(IRef x, IRef f, int64_t levels);

		IRef EmitGenerator(IROpCode::Enum op, Type::Enum type, int64_t length, int64_t a, int64_t b);
		IRef EmitRandom(int64_t length);
		IRef EmitRepeat(int64_t length, int64_t a, int64_t b);
		IRef EmitSequence(int64_t length, int64_t a, int64_t b);
		IRef EmitSequence(int64_t length, double a, double b);
		IRef EmitConstant(Type::Enum type, int64_t length, int64_t c);
		IRef EmitGather(Value const& v, IRef i);
		IRef EmitLoad(Value const& v, int64_t length, int64_t offset);
		IRef EmitSLoad(Value const& v);
		IRef EmitSStore(IRef ref, int64_t index, IRef value);

		IRef GetRef(Value const& v) {
			if(v.isFuture()) return v.future.ref;
			else if(v.length == 1) return EmitConstant(v.type, 1, v.i);
			else return EmitLoad(v,v.length,0);
		}

		void Execute(Thread & thread);
		void Execute(Thread & thread, IRef ref);
		void Reset();

	private:
		void WriteOutputs(Thread & thread);
		std::string toString(Thread & thread);

		void Interpret(Thread & thread);
		void Optimize(Thread& thread);
		void JIT(Thread & thread);

		void MarkLiveOutputs(Thread& thread);
		void SimplifyOps(Thread& thread);
		void AlgebraicSimplification(Thread& thread);
		void CSEElimination(Thread& thread);
		void UsePropogation(Thread& thread);
		void DefPropogation(Thread& thread);
		void DeadCodeElimination(Thread& thread);
		void PropogateShape(IRNode::Shape shape, IRNode& node);
		void ShapePropogation(Thread& thread);
};

#endif

#define DEFAULT_NUM_REGISTERS 10000

#ifdef USE_LLVM_COMPILER
struct LLVMState;
#endif

////////////////////////////////////////////////////////////////////
// Global shared state 
///////////////////////////////////////////////////////////////////

class State : public gc {
public:
	StringTable strings;
	
	std::vector<InternalFunction> internalFunctions;
	std::map<String, int64_t> internalFunctionIndex;
	
	std::vector<Environment*, traceable_allocator<Environment*> > path;
	Environment* global;

	std::vector<Thread*, traceable_allocator<Thread*> > threads;
	int64_t nThreads;

	bool verbose;
	bool jitEnabled;

	int64_t done;

	Character arguments;

	State(uint64_t threads, int64_t argc, char** argv);

	~State() {
		fetch_and_add(&done, 1);
		while(fetch_and_add(&done, 0) != nThreads) { sleep(); }
	}


	Thread& getMainThread() const {
		return *threads[0];
	}

	void registerInternalFunction(String s, InternalFunctionPtr internalFunction, int64_t params) {
		InternalFunction i = { internalFunction, params };
		internalFunctions.push_back(i);
		internalFunctionIndex[s] = internalFunctions.size()-1;
	}

	void interpreter_init(Thread& state);
	
	std::string stringify(Value const& v) const;
#ifdef ENABLE_JIT
	std::string stringify(Trace const & t) const;
#endif
	std::string deparse(Value const& v) const;

	String internStr(std::string s) {
		return strings.in(s);
	}

	std::string externStr(String s) const {
		return strings.out(s);
	}

#ifdef USE_LLVM_COMPILER
    LLVMState * llvmState;
#endif
};

////////////////////////////////////////////////////////////////////
// Per-thread state 
///////////////////////////////////////////////////////////////////

typedef void* (*TaskHeaderPtr)(void* args, uint64_t a, uint64_t b, Thread& thread);
typedef void (*TaskFunctionPtr)(void* args, void* header, uint64_t a, uint64_t b, Thread& thread);

class Thread : public gc {
public:
	struct Task : public gc {
		TaskHeaderPtr header;
		TaskFunctionPtr func;
		void* args;
		uint64_t a;	// start of range [a <= x < b]
		uint64_t b;	// end
		uint64_t alignment;
		uint64_t ppt;
		int64_t* done;
		Task() : func(0), args(0), done(0) {}
		Task(TaskHeaderPtr header, TaskFunctionPtr func, void* args, uint64_t a, uint64_t b, uint64_t alignment, uint64_t ppt) 
			: header(header), func(func), args(args), a(a), b(b), alignment(alignment), ppt(ppt) {
			done = new (GC) int64_t(1);
		}
	};

	State& state;
	uint64_t index;
	pthread_t thread;
	
	Value* base;
	Value* registers;

	std::vector<StackFrame, traceable_allocator<StackFrame> > stack;
	StackFrame frame;
	std::vector<Environment*, traceable_allocator<Environment*> > environments;

	std::vector<std::string> warnings;

#ifdef ENABLE_JIT
private:
	std::vector<Trace*, traceable_allocator<Trace*> > availableTraces;
	std::map<int64_t, Trace*, std::less<int64_t>, traceable_allocator<std::pair<int64_t, Trace*> > > traces;
public:
#endif

	std::deque<Task> tasks;
	Lock tasksLock;
	int64_t steals;

	int64_t assignment[64], set[64]; // temporary space for matching arguments
	
	struct RandomSeed {
		uint64_t v[2];
		uint64_t m[2];
		uint64_t a[2];
		uint64_t padding[10];
	};

	static RandomSeed seed[100];

	Thread(State& state, uint64_t index);

	StackFrame& push() {
		stack.push_back(frame);
		return frame;
	}

	void pop() {
		frame = stack.back();
		stack.pop_back();
	}

	std::string stringify(Value const& v) const { return state.stringify(v); }
#ifdef ENABLE_JIT
	std::string stringify(Trace const & t) const { return state.stringify(t); }
#endif
	std::string deparse(Value const& v) const { return state.deparse(v); }
	String internStr(std::string s) { return state.internStr(s); }
	std::string externStr(String s) const { return state.externStr(s); }

	static void* start(void* ptr) {
		Thread* p = (Thread*)ptr;
		p->loop();
		return 0;
	}

	Value eval(Prototype const* prototype, Environment* environment); 
	Value eval(Prototype const* prototype);
	
	void doall(TaskHeaderPtr header, TaskFunctionPtr func, void* args, uint64_t a, uint64_t b, uint64_t alignment=1, uint64_t ppt = 1) {
		if(a < b && func != 0) {
			uint64_t tmp = ppt+alignment-1;
			ppt = std::max((uint64_t)1, tmp - (tmp % alignment));

			Task t(header, func, args, a, b, alignment, ppt);
			run(t);
	
			while(fetch_and_add(t.done, 0) != 0) {
				Task s;
				if(dequeue(s) || steal(s)) run(s);
				else sleep(); 
			}
		}
	}

private:
	void loop() {
		while(fetch_and_add(&(state.done), 0) == 0) {
			// pull stuff off my queue and run
			// or steal and run
			Task s;
			if(dequeue(s) || steal(s)) {
				try {
					run(s);
				} catch(RiposteError& error) {
					printf("Error (riposte:%d): %s\n", (int)index, error.what().c_str());
				} catch(RuntimeError& error) {
					printf("Error (runtime:%d): %s\n", (int)index, error.what().c_str());
				} catch(CompileError& error) {
					printf("Error (compiler:%d): %s\n", (int)index, error.what().c_str());
				}
			} else sleep(); 
		}
		fetch_and_add(&(state.done), 1);
	}

	void run(Task& t) {
		void* h = t.header != NULL ? t.header(t.args, t.a, t.b, *this) : 0;
		while(t.a < t.b) {
			// check if we need to relinquish some of our chunk...
			int64_t s = atomic_xchg(&steals, 0);
			if(s > 0 && (t.b-t.a) > t.ppt) {
				Task n = t;
				if((t.b-t.a) > t.ppt*4) {
					uint64_t half = split(t);
					t.b = half;
					n.a = half;
				} else {
					t.b = t.a+t.ppt;
					n.a = t.a+t.ppt;
				}
				if(n.a < n.b) {
					//printf("Thread %d relinquishing %d (%d %d)\n", index, n.b-n.a, t.a, t.b);
					tasksLock.acquire();
					fetch_and_add(t.done, 1); 
					tasks.push_front(n);
					tasksLock.release();
				}
			}
			t.func(t.args, h, t.a, std::min(t.a+t.ppt,t.b), *this);
			t.a += t.ppt;
		}
		//printf("Thread %d finished %d %d (%d)\n", index, t.a, t.b, t.done);
		fetch_and_add(t.done, -1);
	}

	uint64_t split(Task const& t) {
		uint64_t half = (t.a+t.b)/2;
		uint64_t r = half + (t.alignment/2);
		half = r - (r % t.alignment);
		if(half < t.a) half = t.a;
		if(half > t.b) half = t.b;
		return half;
	}

	bool dequeue(Task& out) {
		tasksLock.acquire();
		if(tasks.size() >= 1) {
			out = tasks.front();
			tasks.pop_front();
			tasksLock.release();
			return true;
		}
		tasksLock.release();
		return false;
	}

	bool steal(Task& out) {
		// check other threads for available tasks, don't check myself.
		bool found = false;
		for(uint64_t i = 0; i < state.threads.size() && !found; i++) {
			if(i != index) {
				Thread& t = *(state.threads[i]);
				t.tasksLock.acquire();
				if(t.tasks.size() > 0) {
					out = t.tasks.back();
					t.tasks.pop_back();
					t.tasksLock.release();
					found = true;
				} else {
					fetch_and_add(&t.steals,1);
					t.tasksLock.release();
				}
			}
		}
		return found;
	}

public:
	Type::Enum futureType(Value const& v) const {
		if(v.isFuture()) return v.future.typ;
		else return v.type;
	}

	IRNode::Shape futureShape(Value const& v) const {
		if(v.isFuture()) {
			return traces.find(v.length)->second->nodes[v.future.ref].outShape;
		}
		else 
			return (IRNode::Shape) { v.length, -1, 1, -1 };
	}

	Trace* getTrace(int64_t length) {
		if(traces.find(length) == traces.end()) {
			if(availableTraces.size() == 0) {
				Trace* t = new (GC) Trace();
				availableTraces.push_back(t);
			}
			Trace* t = availableTraces.back();
			t->Reset();
			t->Size = length;
			traces[length] = t;
			availableTraces.pop_back();
		}
		return traces[length];
	}

	Trace* getTrace(Value const& a) {
		return getTrace(a.length);
	}

	Trace* getTrace(Value const& a, Value const& b) {
		int64_t la = a.length;
		int64_t lb = b.length;
		if(la == lb || la == 1)
			return getTrace(lb);
		else if(lb == 1)
			return getTrace(la);
		else
			_error("Shouldn't get here");
	}

	Trace* getTrace(Value const& a, Value const& b, Value const& c) {
		int64_t la = a.length;
		int64_t lb = b.length;
		int64_t lc = c.length;
		if(la != 1)
			return getTrace(la);
		else if(lb != 1)
			return getTrace(lb);
		else if(lc != 1)
			return getTrace(lc);
		else
			_error("Shouldn't get here");
	}

	template< template<class X> class Group >
	Value EmitUnary(Environment* env, IROpCode::Enum op, Value const& a, int64_t data) {
		IRef r;
		Trace* trace = getTrace(a);
		trace->liveEnvironments.insert(env);
		if(futureType(a) == Type::Double) {
			r = trace->EmitUnary(op, Group<Double>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Double>::MA::VectorType), data);
		} else if(futureType(a) == Type::Integer) {
			r = trace->EmitUnary(op, Group<Integer>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Integer>::MA::VectorType), data);
		} else if(futureType(a) == Type::Logical) {
			r = trace->EmitUnary(op, Group<Logical>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Logical>::MA::VectorType), data);
		} else _error("Attempting to record invalid type in EmitUnary");
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	template< template<class X, class Y> class Group >
	Value EmitBinary(Environment* env, IROpCode::Enum op, Value const& a, Value const& b, int64_t data) {
		IRef r;
		Trace* trace = getTrace(a,b);
		trace->liveEnvironments.insert(env);
		if(futureType(a) == Type::Double) {
			if(futureType(b) == Type::Double)
				r = trace->EmitBinary(op, Group<Double,Double>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Double,Double>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Double,Double>::MB::VectorType), data);
			else if(futureType(b) == Type::Integer)
				r = trace->EmitBinary(op, Group<Double,Integer>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Double,Integer>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Double,Integer>::MB::VectorType), data);
			else if(futureType(b) == Type::Logical)
				r = trace->EmitBinary(op, Group<Double,Logical>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Double,Logical>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Double,Logical>::MB::VectorType), data);
			else _error("Attempting to record invalid type in EmitBinary");
		} else if(futureType(a) == Type::Integer) {
			if(futureType(b) == Type::Double)
				r = trace->EmitBinary(op, Group<Integer,Double>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Integer,Double>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Integer,Double>::MB::VectorType), data);
			else if(futureType(b) == Type::Integer)
				r = trace->EmitBinary(op, Group<Integer,Integer>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Integer,Integer>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Integer,Integer>::MB::VectorType), data);
			else if(futureType(b) == Type::Logical)
				r = trace->EmitBinary(op, Group<Integer,Logical>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Integer,Logical>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Integer,Logical>::MB::VectorType), data);
			else _error("Attempting to record invalid type in EmitBinary");
		} else if(futureType(a) == Type::Logical) {
			if(futureType(b) == Type::Double)
				r = trace->EmitBinary(op, Group<Logical,Double>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Logical,Double>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Logical,Double>::MB::VectorType), data);
			else if(futureType(b) == Type::Integer)
				r = trace->EmitBinary(op, Group<Logical,Integer>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Logical,Integer>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Logical,Integer>::MB::VectorType), data);
			else if(futureType(b) == Type::Logical)
				r = trace->EmitBinary(op, Group<Logical,Logical>::R::VectorType, trace->EmitCoerce(trace->GetRef(a), Group<Logical,Logical>::MA::VectorType), trace->EmitCoerce(trace->GetRef(b), Group<Logical,Logical>::MB::VectorType), data);
			else _error("Attempting to record invalid type in EmitBinary");
		} else _error("Attempting to record invalid type in EmitBinary");
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	Value EmitSplit(Environment* env, Value const& a, Value const& b, int64_t data) {
		Trace* trace = getTrace(a,b);
		trace->liveEnvironments.insert(env);
		IRef r = trace->EmitSplit(trace->GetRef(a), trace->EmitCoerce(trace->GetRef(b), Type::Integer), data);
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	Value EmitConstant(Environment* env, Type::Enum type, int64_t length, int64_t c) {
		Trace* trace = getTrace(length);
		trace->liveEnvironments.insert(env);
		IRef r = trace->EmitConstant(type, length, c);
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	Value EmitRandom(Environment* env, int64_t length) {
		Trace* trace = getTrace(length);
		trace->liveEnvironments.insert(env);
		IRef r = trace->EmitRandom(length);
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	Value EmitRepeat(Environment* env, int64_t length, int64_t a, int64_t b) {
		Trace* trace = getTrace(length);
		trace->liveEnvironments.insert(env);
		IRef r = trace->EmitBinary(IROpCode::add, Type::Integer, trace->EmitRepeat(length, a, b), trace->EmitConstant(Type::Integer, length, 1), 0);
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}
		
	Value EmitSequence(Environment* env, int64_t length, int64_t a, int64_t b) {
		Trace* trace = getTrace(length);
		trace->liveEnvironments.insert(env);
		IRef r = trace->EmitSequence(length, a, b);
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	Value EmitSequence(Environment* env, int64_t length, double a, double b) {
		Trace* trace = getTrace(length);
		trace->liveEnvironments.insert(env);
		IRef r = trace->EmitSequence(length, a, b);
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	Value EmitGather(Environment* env, Value const& a, Value const& i) {
		Trace* trace = getTrace(i);
		trace->liveEnvironments.insert(env);
		IRef o = trace->EmitConstant(Type::Integer, 1, 1);
		IRef im1 = trace->EmitBinary(IROpCode::sub, Type::Integer, trace->EmitCoerce(trace->GetRef(i), Type::Integer), o, 0);
		IRef r = trace->EmitGather(a, im1);
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	Value EmitFilter(Environment* env, Value const& a, Value const& i) {
		Trace* trace = getTrace(a);
		trace->liveEnvironments.insert(env);
		IRef r = trace->EmitFilter(trace->GetRef(a), trace->EmitCoerce(trace->GetRef(i), Type::Logical));
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}

	Value EmitIfElse(Environment* env, Value const& a, Value const& b, Value const& cond) {
		Trace* trace = getTrace(a,b,cond);
		trace->liveEnvironments.insert(env);
		
		IRef r = trace->EmitIfElse( 	trace->GetRef(a),
						trace->GetRef(b),
						trace->GetRef(cond));
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}
	
	Value EmitSStore(Environment* env, Value const& a, int64_t index, Value const& b) {
		Trace* trace = getTrace(b);
		trace->liveEnvironments.insert(env);
		
		IRef m = a.isFuture() ? a.future.ref : trace->EmitSLoad(a);

		IRef r = trace->EmitSStore(m, index, trace->GetRef(b));
		
		Value v;
		Future::Init(v, trace->nodes[r].type, trace->nodes[r].shape.length, r);
		return v;
	}
	
	void LiveEnvironment(Environment* env, Value const& a) {
		if(a.isFuture()) {
			Trace* trace = getTrace(a);
			trace->liveEnvironments.insert(env);
		}
	}

	void KillEnvironment(Environment* env) {
		for(std::map<int64_t, Trace*, std::less<int64_t>, traceable_allocator<std::pair<int64_t, Trace*> > >::const_iterator i = traces.begin(); i != traces.end(); i++) {
			i->second->liveEnvironments.erase(env);
		}
	}

	void Bind(Value const& v) {
		if(!v.isFuture()) return;
		std::map<int64_t, Trace*, std::less<int64_t>, traceable_allocator<std::pair<int64_t, Trace*> > >::iterator i = traces.find(v.length);
		if(i == traces.end()) 
			_error("Unevaluated future left behind");
		Trace* trace = i->second;
		trace->Execute(*this, v.future.ref);
		trace->Reset();
		availableTraces.push_back(trace);
		traces.erase(i);
	}

	void Flush(Thread & thread) {
		// execute all traces
		for(std::map<int64_t, Trace*, std::less<int64_t>, traceable_allocator<std::pair<int64_t, Trace*> > >::const_iterator i = traces.begin(); i != traces.end(); i++) {
			Trace* trace = i->second;
			trace->Execute(*this);
			trace->Reset();
			availableTraces.push_back(trace);
		}
		traces.clear();
	}

	void OptBind(Value const& v) {
		if(!v.isFuture()) return;
		std::map<int64_t, Trace*, std::less<int64_t>, traceable_allocator<std::pair<int64_t, Trace*> > >::iterator i = traces.find(v.length);
		if(i == traces.end()) 
			_error("Unevaluated future left behind");
		Trace* trace = i->second;
		if(trace->nodes.size() > 2048) {
			Bind(v);
		}
	}
};

#ifdef USE_LLVM_COMPILER
void TraceLLVMCompiler_init(State & state);
#endif

inline State::State(uint64_t threads, int64_t argc, char** argv) 
	: nThreads(threads), verbose(false), jitEnabled(true), done(0) {
	Environment* base = new (GC) Environment(0);
	this->global = new (GC) Environment(base);
	path.push_back(base);

	arguments = Character(argc);
	for(int64_t i = 0; i < argc; i++) {
		arguments[i] = internStr(std::string(argv[i]));
	}
	
	pthread_attr_t  attr;
	pthread_attr_init (&attr);
	pthread_attr_setscope (&attr, PTHREAD_SCOPE_SYSTEM);
	pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);

	Thread* t = new (GC) Thread(*this, 0);
	this->threads.push_back(t);

	for(uint64_t i = 1; i < threads; i++) {
		Thread* t = new Thread(*this, i);
		pthread_create (&t->thread, &attr, Thread::start, t);
		this->threads.push_back(t);
	}

#ifdef USE_LLVM_COMPILER
    TraceLLVMCompiler_init(*this);
#endif
    
	interpreter_init(getMainThread());
}

#endif
