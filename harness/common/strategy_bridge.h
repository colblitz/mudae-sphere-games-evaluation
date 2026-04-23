/**
 * strategy_bridge.h — Language-dispatch bridge for loading strategies.
 *
 * Supports three languages detected from file extension:
 *
 *   .py   Python 3.11+  — embedded via pybind11 / Python C API.
 *         The .py file must define a class that subclasses the appropriate
 *         interface ABC from interface/strategy.py.  The harness imports
 *         the module, finds the subclass, and calls its methods directly.
 *
 *   .cpp  C++           — compiled to a shared library (.so) automatically
 *         by the harness, then loaded via dlopen.  The .cpp file must export
 *         extern "C" create_strategy() and destroy_strategy().
 *
 *   .js   JavaScript    — spawned as a Node.js child process.  Communication
 *         uses newline-delimited JSON on stdin/stdout (see strategy.js).
 *
 * Usage:
 *   auto bridge = StrategyBridge::load("strategies/oc/my_strategy.py", "oc");
 *   bridge->init_evaluation_run();
 *   bridge->init_game_payload(meta_json);
 *   Click c = bridge->next_click(board, meta_json);
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// pybind11 / Python C API
#include <Python.h>

// dlopen for C++ strategies
#include <dlfcn.h>

// pipe / fork / waitpid for JS strategies
#include <cerrno>
#include <csignal>
#include <ctime>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "types.h"
#include "json_proto.h"

namespace sphere {

// ---------------------------------------------------------------------------
// Abstract bridge interface
// ---------------------------------------------------------------------------

class StrategyBridge {
public:
    virtual ~StrategyBridge() = default;

    virtual std::string init_evaluation_run()                                                   = 0;
    virtual std::string init_game_payload(const std::string& meta_json,
                                          const std::string& evaluation_run_state_json)        = 0;
    virtual Click       next_click(const std::vector<Cell>& board,
                                   const std::string&        meta_json,
                                   const std::string&        game_state_json)                  = 0;

    // Returns the updated game state JSON from the most recent next_click call.
    // Subclasses must set last_game_state_json_ before returning from next_click.
    // Callers must thread this back in as game_state_json on the next call.
    const std::string& last_game_state() const { return last_game_state_json_; }

    // Factory: detects language from extension and constructs the right bridge.
    // game_name is one of "oh", "oc", "oq", "ot" (used to find the ABC class).
    static std::unique_ptr<StrategyBridge> load(const std::string& path,
                                                const std::string& game_name);

protected:
    std::string last_game_state_json_ = "null";
};

// ---------------------------------------------------------------------------
// Python bridge (pybind11 / Python C API)
// ---------------------------------------------------------------------------

class PythonBridge : public StrategyBridge {
public:
    PythonBridge(const std::string& path, const std::string& game_name) {
        // Ensure interpreter is initialised (idempotent)
        if (!Py_IsInitialized()) {
            Py_InitializeEx(0);
            // Add the repo root to sys.path so interface/ is importable.
            // REPO_ROOT is injected at compile time via -DREPO_ROOT=\"...\"
            PyRun_SimpleString(
                "import sys\n"
                "if '" REPO_ROOT "' not in sys.path: sys.path.insert(0, '" REPO_ROOT "')\n"
            );
        }

        // Determine the directory containing the strategy file and add it to sys.path
        std::string dir = path.substr(0, path.rfind('/'));
        if (dir.empty()) dir = ".";
        std::string add_path =
            "import sys\n"
            "if '" + dir + "' not in sys.path: sys.path.insert(0, '" + dir + "')\n";
        PyRun_SimpleString(add_path.c_str());

        // Import the module (file stem = module name)
        std::string stem = path.substr(path.rfind('/') + 1);
        if (stem.rfind(".py") == stem.size() - 3) stem = stem.substr(0, stem.size() - 3);

        PyObject* mod = PyImport_ImportModule(stem.c_str());
        if (!mod) {
            PyErr_Print();
            throw std::runtime_error("PythonBridge: failed to import " + path);
        }

        // Find a class that is a subclass of the appropriate ABC
        // ABC class names: OHStrategy, OCStrategy, OQStrategy, OTStrategy
        std::string abc_name;
        for (char c : game_name) abc_name += static_cast<char>(toupper(c));
        abc_name += "Strategy";

        // Import the interface module to get the ABC
        PyObject* iface_mod = PyImport_ImportModule("interface.strategy");
        if (!iface_mod) {
            PyErr_Clear();
            // Try direct import path
            PyRun_SimpleString(
                "import sys, os\n"
                "_iface_dir = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath"
                "(__file__ if hasattr(__builtins__, '__file__') else '.')), '../..'))\n"
                "if _iface_dir not in sys.path: sys.path.insert(0, _iface_dir)\n"
            );
            iface_mod = PyImport_ImportModule("interface.strategy");
        }
        if (!iface_mod) {
            Py_DECREF(mod);
            PyErr_Print();
            throw std::runtime_error("PythonBridge: failed to import interface.strategy");
        }

        PyObject* abc_cls = PyObject_GetAttrString(iface_mod, abc_name.c_str());
        Py_DECREF(iface_mod);
        if (!abc_cls) {
            Py_DECREF(mod);
            throw std::runtime_error("PythonBridge: no class " + abc_name + " in interface.strategy");
        }

        // Iterate module attributes to find a concrete subclass
        PyObject* mod_dict = PyModule_GetDict(mod);
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        PyObject* strategy_cls = nullptr;
        while (PyDict_Next(mod_dict, &pos, &key, &val)) {
            if (!PyType_Check(val)) continue;
            if (val == abc_cls) continue;
            if (PyObject_IsSubclass(val, abc_cls) == 1) {
                strategy_cls = val;
                break;
            }
        }
        Py_DECREF(abc_cls);
        Py_DECREF(mod);

        if (!strategy_cls) {
            throw std::runtime_error(
                "PythonBridge: no concrete subclass of " + abc_name + " found in " + path);
        }

        // Instantiate the strategy
        instance_ = PyObject_CallObject(strategy_cls, nullptr);
        if (!instance_) {
            PyErr_Print();
            throw std::runtime_error("PythonBridge: failed to instantiate strategy");
        }
    }

    ~PythonBridge() override {
        Py_XDECREF(instance_);
    }

    std::string init_evaluation_run() override {
        PyGILState_STATE gstate = PyGILState_Ensure();
        PyObject* ret = PyObject_CallMethod(instance_, "init_evaluation_run", nullptr);
        if (!ret) { PyErr_Clear(); PyGILState_Release(gstate); return "null"; }
        std::string s = py_to_json(ret);
        Py_DECREF(ret);
        PyGILState_Release(gstate);
        return s;
    }

    std::string init_game_payload(const std::string& meta_json,
                                  const std::string& evaluation_run_state_json) override {
        PyGILState_STATE gstate = PyGILState_Ensure();
        PyObject* meta  = json_to_py(meta_json);
        PyObject* ers   = json_to_py(evaluation_run_state_json);
        PyObject* ret   = PyObject_CallMethod(instance_, "init_game_payload", "OO", meta, ers);
        Py_DECREF(meta);
        Py_DECREF(ers);
        if (!ret) { PyErr_Print(); PyGILState_Release(gstate); return evaluation_run_state_json; }
        std::string s = py_to_json(ret);
        Py_DECREF(ret);
        PyGILState_Release(gstate);
        return s;
    }

    Click next_click(const std::vector<Cell>& board,
                     const std::string&        meta_json,
                     const std::string&        game_state_json) override {
        PyGILState_STATE gstate = PyGILState_Ensure();
        PyObject* rev        = cells_to_py(board);
        PyObject* meta       = json_to_py(meta_json);
        PyObject* game_state = json_to_py(game_state_json);
        PyObject* ret        = PyObject_CallMethod(instance_, "next_click", "OOO", rev, meta, game_state);
        Py_DECREF(rev);
        Py_DECREF(meta);
        Py_DECREF(game_state);
        if (!ret) {
            PyErr_Print();
            PyGILState_Release(gstate);
            throw std::runtime_error("PythonBridge: next_click raised an exception (traceback above)");
        }
        Click c = py_to_click(ret);
        // Extract updated game_state_json from ret[2] into the base class field
        PyObject* ns = PyTuple_GetItem(ret, 2);
        if (ns) {
            last_game_state_json_ = py_to_json(ns);
        }
        Py_DECREF(ret);
        PyGILState_Release(gstate);
        return c;
    }

private:
    PyObject* instance_ = nullptr;

    // Convert std::vector<Cell> (full 25-cell board) → Python list of dicts
    static PyObject* cells_to_py(const std::vector<Cell>& cells) {
        PyObject* lst = PyList_New(static_cast<Py_ssize_t>(cells.size()));
        for (size_t i = 0; i < cells.size(); ++i) {
            PyObject* d = PyDict_New();
            PyDict_SetItemString(d, "row",     PyLong_FromLong(cells[i].row));
            PyDict_SetItemString(d, "col",     PyLong_FromLong(cells[i].col));
            PyDict_SetItemString(d, "color",   PyUnicode_FromString(cells[i].color.c_str()));
            PyDict_SetItemString(d, "clicked", PyBool_FromLong(cells[i].clicked ? 1 : 0));
            PyList_SET_ITEM(lst, static_cast<Py_ssize_t>(i), d);
        }
        return lst;
    }

    // Parse (row, col, state) from Python return value
    static Click py_to_click(PyObject* ret) {
        Click c{0, 0};
        if (!ret || !PyTuple_Check(ret) || PyTuple_Size(ret) < 2) return c;
        PyObject* r = PyTuple_GetItem(ret, 0);
        PyObject* cl = PyTuple_GetItem(ret, 1);
        if (r)  c.row = static_cast<int8_t>(PyLong_AsLong(r));
        if (cl) c.col = static_cast<int8_t>(PyLong_AsLong(cl));
        return c;
    }

    // Python object → JSON string (via json.dumps)
    static std::string py_to_json(PyObject* obj) {
        static PyObject* dumps = nullptr;
        if (!dumps) {
            PyObject* json_mod = PyImport_ImportModule("json");
            if (json_mod) { dumps = PyObject_GetAttrString(json_mod, "dumps"); Py_DECREF(json_mod); }
        }
        if (!dumps) return "null";
        PyObject* ret = PyObject_CallFunctionObjArgs(dumps, obj, nullptr);
        if (!ret) { PyErr_Clear(); return "null"; }
        std::string s = PyUnicode_AsUTF8(ret);
        Py_DECREF(ret);
        return s;
    }

    // JSON string → Python object (via json.loads)
    static PyObject* json_to_py(const std::string& json) {
        static PyObject* loads = nullptr;
        if (!loads) {
            PyObject* json_mod = PyImport_ImportModule("json");
            if (json_mod) { loads = PyObject_GetAttrString(json_mod, "loads"); Py_DECREF(json_mod); }
        }
        if (!loads) Py_RETURN_NONE;
        PyObject* s   = PyUnicode_FromString(json.c_str());
        PyObject* ret = PyObject_CallFunctionObjArgs(loads, s, nullptr);
        Py_DECREF(s);
        if (!ret) { PyErr_Clear(); Py_RETURN_NONE; }
        return ret;
    }
};

// ---------------------------------------------------------------------------
// C++ bridge (dlopen)
// ---------------------------------------------------------------------------

// Function pointer types matching interface/strategy.h extern "C" exports
using CreateFn  = void* (*)();
using DestroyFn = void (*)(void*);

// Minimal vtable-free proxy: we call the C++ strategy methods via a
// thin JSON adapter defined in the compiled .so itself.  The .so must export:
//   extern "C" void*       create_strategy();
//   extern "C" void        destroy_strategy(void*);
//   extern "C" const char* strategy_init_evaluation_run(void*);
//   extern "C" const char* strategy_init_game_payload(void*, const char* meta, const char* state);
//   extern "C" const char* strategy_next_click(void*, const char* revealed_json,
//                                               const char* meta, const char* state);
// The last returned string must remain valid until the next call to the same function.

using InitPayloadFn  = const char* (*)(void*);
using InitRunFn      = const char* (*)(void*, const char*, const char*);
using NextClickFn    = const char* (*)(void*, const char*, const char*, const char*);

class CppBridge : public StrategyBridge {
public:
    CppBridge(const std::string& so_path) {
        handle_ = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            throw std::runtime_error(std::string("CppBridge: dlopen failed: ") + dlerror());
        }
        create_fn_  = reinterpret_cast<CreateFn>(dlsym(handle_, "create_strategy"));
        destroy_fn_ = reinterpret_cast<DestroyFn>(dlsym(handle_, "destroy_strategy"));
        ip_fn_      = reinterpret_cast<InitPayloadFn>(dlsym(handle_, "strategy_init_evaluation_run"));
        ir_fn_      = reinterpret_cast<InitRunFn>(dlsym(handle_, "strategy_init_game_payload"));
        nc_fn_      = reinterpret_cast<NextClickFn>(dlsym(handle_, "strategy_next_click"));
        if (!create_fn_ || !destroy_fn_ || !ip_fn_ || !ir_fn_ || !nc_fn_) {
            dlclose(handle_);
            throw std::runtime_error("CppBridge: missing required export(s) in " + so_path);
        }
        instance_ = create_fn_();
    }

    ~CppBridge() override {
        if (instance_ && destroy_fn_) destroy_fn_(instance_);
        if (handle_) dlclose(handle_);
    }

    std::string init_evaluation_run() override {
        const char* r = ip_fn_(instance_);
        return r ? r : "null";
    }

    std::string init_game_payload(const std::string& meta_json,
                                  const std::string& evaluation_run_state_json) override {
        const char* r = ir_fn_(instance_, meta_json.c_str(), evaluation_run_state_json.c_str());
        return r ? r : evaluation_run_state_json;
    }

    Click next_click(const std::vector<Cell>& board,
                     const std::string&        meta_json,
                     const std::string&        game_state_json) override {
        std::string rev_json = cells_to_json(board);
        const char* r = nc_fn_(instance_, rev_json.c_str(), meta_json.c_str(), game_state_json.c_str());
        if (!r) return {0, 0};
        // Extract updated game_state_json from the JSON response
        last_game_state_json_ = json_parse_state(r);
        return json_parse_click(r);
    }

private:
    void*         handle_     = nullptr;
    void*         instance_   = nullptr;
    CreateFn      create_fn_  = nullptr;
    DestroyFn     destroy_fn_ = nullptr;
    InitPayloadFn ip_fn_      = nullptr;
    InitRunFn     ir_fn_      = nullptr;
    NextClickFn   nc_fn_      = nullptr;

    static std::string cells_to_json(const std::vector<Cell>& cells) {
        std::string s = "[";
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) s += ',';
            s += "{\"row\":" + std::to_string(cells[i].row)
               + ",\"col\":" + std::to_string(cells[i].col)
               + ",\"color\":\"" + cells[i].color + "\""
               + ",\"clicked\":" + (cells[i].clicked ? "true" : "false") + "}";
        }
        s += "]";
        return s;
    }

    static Click json_parse_click(const char* s) {
        // Minimal parser: find "row": N, "col": N
        Click c{0, 0};
        const char* p = strstr(s, "\"row\":");
        if (p) c.row = static_cast<int8_t>(atoi(p + 6));
        p = strstr(s, "\"col\":");
        if (p) c.col = static_cast<int8_t>(atoi(p + 6));
        return c;
    }

    // Extract the "game_state" field from a C++ strategy JSON response.
    // The C++ interface returns {"row":N,"col":N,"game_state":"<json>"}.
    static std::string json_parse_state(const char* s) {
        const char* key = "\"game_state\":";
        const char* p = strstr(s, key);
        if (!p) return "null";
        p += strlen(key);
        while (*p == ' ') ++p;
        if (!*p) return "null";
        // Value is either a quoted string or a raw JSON value
        if (*p == '"') {
            // Unescape the inner JSON string
            ++p;
            std::string result;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p+1)) { result += *(++p); }
                else result += *p;
                ++p;
            }
            return result;
        }
        // Raw value: take until end of object
        const char* end = p + strlen(p) - 1;
        while (end > p && (*end == '}' || *end == ' ' || *end == '\n')) --end;
        return std::string(p, end - p + 1);
    }
};

// ---------------------------------------------------------------------------
// JS bridge (Node.js subprocess, newline-delimited JSON)
// ---------------------------------------------------------------------------

class JsBridge : public StrategyBridge {
public:
    explicit JsBridge(const std::string& path) {
        // Verify node is on PATH before forking
        if (system("node --version >/dev/null 2>&1") != 0) {
            throw std::runtime_error(
                "JsBridge: 'node' not found on PATH. "
                "Install Node.js (e.g. 'sudo apt install nodejs' or via https://nodejs.org).");
        }

        // Pipe setup: parent writes to child stdin, reads from child stdout
        int to_child[2], from_child[2];
        if (pipe(to_child) || pipe(from_child)) {
            throw std::runtime_error("JsBridge: pipe() failed");
        }
        pid_ = fork();
        if (pid_ < 0) throw std::runtime_error("JsBridge: fork() failed");
        if (pid_ == 0) {
            // Child process
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            close(to_child[0]); close(to_child[1]);
            close(from_child[0]); close(from_child[1]);
            execlp("node", "node", path.c_str(), nullptr);
            _exit(127);  // exec failed
        }
        // Parent
        close(to_child[0]);
        close(from_child[1]);
        write_fd_ = to_child[1];
        read_fd_  = from_child[0];
        // Open a FILE* for line-at-a-time reading
        read_fp_  = fdopen(read_fd_, "r");
    }

    ~JsBridge() override {
        if (write_fd_ >= 0) close(write_fd_);
        if (read_fp_)       fclose(read_fp_);
        if (pid_ > 0)       waitpid(pid_, nullptr, 0);
    }

    std::string init_evaluation_run() override {
        std::string msg = "{\"method\":\"init_evaluation_run\"}\n";
        write_line(msg);
        std::string resp = read_line();
        // Extract "value" field
        return extract_json_field(resp, "value");
    }

    std::string init_game_payload(const std::string& meta_json,
                         const std::string& evaluation_run_state_json) override {
        std::string msg = "{\"method\":\"init_game_payload\",\"meta\":" + meta_json
                        + ",\"evaluationRunState\":" + evaluation_run_state_json + "}\n";
        write_line(msg);
        std::string resp = read_line();
        check_error_field(resp, "init_game_payload");
        return extract_json_field(resp, "value");
    }

    Click next_click(const std::vector<Cell>& board,
                     const std::string&        meta_json,
                     const std::string&        game_state_json) override {
        std::string rev_json = cells_to_json(board);
        std::string msg = "{\"method\":\"next_click\",\"board\":" + rev_json
                        + ",\"meta\":" + meta_json
                        + ",\"gameState\":" + game_state_json + "}\n";
        write_line(msg);
        std::string resp = read_line();
        check_error_field(resp, "next_click");
        Click c{0, 0};
        const char* p = strstr(resp.c_str(), "\"row\":");
        if (p) c.row = static_cast<int8_t>(atoi(p + 6));
        p = strstr(resp.c_str(), "\"col\":");
        if (p) c.col = static_cast<int8_t>(atoi(p + 6));
        // Extract the updated gameState from the response into the base class field
        last_game_state_json_ = extract_json_field(resp, "gameState");
        return c;
    }

private:
    pid_t pid_      = -1;
    int   write_fd_ = -1;
    int   read_fd_  = -1;
    FILE* read_fp_  = nullptr;

    // Returns a human-readable description of how the Node child exited.
    // Uses WNOHANG so it never blocks; returns empty string if still running.
    std::string node_exit_description() {
        if (pid_ <= 0) return "";
        int status = 0;
        pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r <= 0) return "(Node process still running or unknown state)";
        pid_ = -1;  // prevent double-wait in destructor
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 127) return "Node process failed to exec (exit 127 — 'node' binary not found?)";
            return "Node process exited with code " + std::to_string(code);
        }
        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            const char* name = strsignal(sig);
            return std::string("Node process killed by signal ")
                 + std::to_string(sig)
                 + " (" + (name ? name : "unknown") + ")";
        }
        return "Node process exited with unknown status";
    }

    void write_line(const std::string& s) {
        const char* p = s.c_str();
        size_t rem    = s.size();
        while (rem > 0) {
            ssize_t n = write(write_fd_, p, rem);
            if (n <= 0) {
                std::string why = node_exit_description();
                throw std::runtime_error(
                    std::string("JsBridge: write to Node stdin failed: ")
                    + strerror(errno)
                    + (why.empty() ? "" : ". " + why));
            }
            p   += n;
            rem -= static_cast<size_t>(n);
        }
    }

    std::string read_line() {
        std::string result;
        result.reserve(4096);
        int c;
        while ((c = fgetc(read_fp_)) != EOF && c != '\n') {
            result += static_cast<char>(c);
        }
        // Strip trailing carriage return if present
        if (!result.empty() && result.back() == '\r') result.pop_back();
        if (c == EOF) {
            // Node process closed its stdout — it crashed or exited unexpectedly.
            std::string why = node_exit_description();
            throw std::runtime_error(
                "JsBridge: Node process closed stdout unexpectedly"
                + (why.empty() ? "" : ". " + why)
                + ". Check that register() is called at the bottom of the strategy "
                  "file and that the strategy does not throw during initialisation. "
                  "Node stderr (if any) appears above.");
        }
        return result;
    }

    // Inspect the response JSON for an "error" field and throw if present.
    static void check_error_field(const std::string& resp, const char* method) {
        std::string err = extract_json_field(resp, "error");
        if (err == "null" || err.empty()) return;
        // Strip surrounding quotes from string value if present
        std::string msg = err;
        if (msg.size() >= 2 && msg.front() == '"' && msg.back() == '"')
            msg = msg.substr(1, msg.size() - 2);
        throw std::runtime_error(
            std::string("JsBridge: strategy error in ") + method + "(): " + msg);
    }

    // Extract the value of a top-level JSON field by key.
    // Handles arbitrarily nested/large values by tracking bracket depth.
    static std::string extract_json_field(const std::string& json, const char* field) {
        std::string key = std::string("\"") + field + "\":";
        size_t pos = json.find(key);
        if (pos == std::string::npos) return "null";
        size_t start = pos + key.size();
        while (start < json.size() && json[start] == ' ') ++start;
        if (start >= json.size()) return "null";

        char first = json[start];

        // String value
        if (first == '"') {
            size_t end = start + 1;
            while (end < json.size()) {
                if (json[end] == '\\') { end += 2; continue; }
                if (json[end] == '"') { ++end; break; }
                ++end;
            }
            return json.substr(start, end - start);
        }

        // Object or array: scan for matching close bracket respecting depth
        if (first == '{' || first == '[') {
            char open = first, close = (first == '{') ? '}' : ']';
            int depth = 0;
            bool in_str = false;
            size_t i = start;
            for (; i < json.size(); ++i) {
                char c = json[i];
                if (in_str) {
                    if (c == '\\') { ++i; continue; }
                    if (c == '"')  in_str = false;
                    continue;
                }
                if (c == '"')   { in_str = true; continue; }
                if (c == open)  { ++depth; continue; }
                if (c == close) { --depth; if (depth == 0) { ++i; break; } }
            }
            return json.substr(start, i - start);
        }

        // Primitive (number, bool, null): read until delimiter
        size_t end = start;
        while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']')
            ++end;
        return json.substr(start, end - start);
    }

    static std::string cells_to_json(const std::vector<Cell>& cells) {
        std::string s = "[";
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) s += ',';
            s += "{\"row\":" + std::to_string(cells[i].row)
               + ",\"col\":" + std::to_string(cells[i].col)
               + ",\"color\":\"" + cells[i].color + "\""
               + ",\"clicked\":" + (cells[i].clicked ? "true" : "false") + "}";
        }
        s += "]";
        return s;
    }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

inline std::unique_ptr<StrategyBridge> StrategyBridge::load(const std::string& path,
                                                             const std::string& game_name) {
    // Detect extension
    size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        throw std::runtime_error("StrategyBridge: cannot detect language for " + path);
    std::string ext = path.substr(dot);

    if (ext == ".py") {
        return std::make_unique<PythonBridge>(path, game_name);
    }

    if (ext == ".cpp") {
        // Compile to .so in a temp location
        std::string so = path.substr(0, path.size() - 4) + ".so";
        std::string cmd =
            "g++ -O2 -march=native -std=c++17 -shared -fPIC"
            " -I" + std::string(REPO_ROOT) + "/interface"
            " -o " + so + " " + path;
        fprintf(stderr, "[harness] compiling %s ...\n", path.c_str());
        int rc = system(cmd.c_str());
        if (rc != 0)
            throw std::runtime_error("StrategyBridge: compilation failed for " + path);
        return std::make_unique<CppBridge>(so);
    }

    if (ext == ".js") {
        return std::make_unique<JsBridge>(path);
    }

    throw std::runtime_error("StrategyBridge: unsupported extension " + ext);
}

}  // namespace sphere
