/*
    Riposte, Copyright (C) 2010-2012 Stanford University.
    
    main.cpp - the REPL loop
*/

#include <fstream>
#include <unistd.h>
#include <getopt.h>

#include "riposte.h"
#include "parser.h"
#include "compiler.h"
#include "library.h"

extern "C" 
{
    #include "../libs/linenoise/linenoise.h"
}

/*  Globals  */
static int debug = 0;
static int verbose = 0;

static void info(int threads, std::ostream& out) 
{
    out << "Riposte (" << threads << " threads) "
        << "-- Copyright (C) 2010-2013 Stanford, 2014 Justin Talbot" << std::endl;
    out << "http://jtalbot.github.com/riposte/" << std::endl;
    out << std::endl;
    out << "If you have the base R libraries installed, you can load them by running:" << std::endl << "source('bootstrap.R')" << std::endl;

}

/* debug messages */
static void d_message(const int level, const char* ifmt, const char *msg)
{
    char*  fmt;
    if (debug < level) return;
    fprintf(stderr,"DEBUG: ");
    fmt = (char *)ifmt;
    if (!fmt) 
        fmt = (char *)"%s\n";
    fprintf(stderr, fmt, msg);
    fflush(stderr);
}
 
 /* error messages */
static void e_message(const char *severity,const char *source, const char *msg)
{
    fprintf(stderr, "%s: (%s) ",severity,source);
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
}
 
 /* verbose  messages */
static void l_message(const int level, const char *msg)
{
    if (level > verbose) return;
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
}

// interactive line entry using linenoise
static bool terminal(Global& global, std::string inname, std::istream & in, std::ostream & out, Value& code)
{ 
    std::string input;
    code = Value::Nil();

    int status = 0;
    while(status == 0)
    {
        char* line = linenoise( input.size() == 0 ? "> " : "+ " );

        if(line == 0)
            return true;

        input += line;

        if(line[0] != '\0') {
            linenoiseHistoryAdd(line);
            linenoiseHistorySave((char*)".riposte_history");
        }

        free(line);
        
        input += "\n";   /* add the discarded newline back in */
        
        if(input.size() > 0) {
            status = parse(global, inname.c_str(),
                input.c_str(), input.size(), true, code);
        }
    }

    if (status == -1) {
        List list(0);
        code = CreateExpression(list);
    }

    return false;
}

// piped in from stream 
static bool pipe(Global& global, std::string inname, std::istream & in, std::ostream & out, Value& code) 
{
    std::string input;
    code = Value::Nil();

    int status = 0;
    while(!in.eof() && status == 0)
    {
        std::string more;
        std::getline(in, more);
        input += more;

        input += "\n";   /* add the discarded newline back in */
        
        if(input.size() > 0) {
            status = parse(global, inname.c_str(),
                input.c_str(), input.size(), true, code);    
        }
    }

    if (status == -1) {
        List list(0);
        code = CreateExpression(list);
    }

    return in.eof();
}

static int run(State& state, std::string inname, std::istream& in, std::ostream& out, bool interactive, bool echo) 
{
    int rc = 0;

    if(interactive) 
    {
        linenoiseHistoryLoad((char*)".riposte_history");
    }

    Code* print;
    if(echo)
    {
        List p(1);
        p[0] = CreateSymbol(state.internStr("repl"));
        //p[1] = CreateSymbol(Strings::Last_value);
        print = Compiler::compileExpression(state, CreateCall(p));
        // save the promise to the gcStack so that the gc doesn't clean it up.
        Value v;
        Promise::Init(v, 0, print, false);
        state.gcStack.push_back(v);
    }

    bool done = false;
    while(!done) {
        try { 
            Value expr;
            done = interactive ?
                terminal(state.global, inname, in, out, expr) :
                pipe(state.global, inname, in, out, expr);

            if(done || (isExpression(expr) && ((List const&)expr).length()==0)) 
                continue;

            Code* code = Compiler::compileExpression(state, expr);
            Value result = state.evalTopLevel(code, state.global.global);

            // Nil indicates an error that was dispatched correctly.
            // Don't print anything, but no need to propagate error.
            if(result.isNil()) 
                continue;

            state.global.global->insert(Strings::Last_value) = result;
            if(echo && state.visible) {
                state.evalTopLevel(print, state.global.global);
                // Print directly (for debugging)
                //std::cout<< state.stringify(result) << std::endl;
            }
            state.visible = true;
        } 
        catch(RiposteException const& e) { 
            e_message("Error", e.kind().c_str(), e.what().c_str());
        } 
    }

    // Clean up after myself
    if(echo) {
        state.gcStack.pop_back();
    }
    
    return rc;
}

static void usage()
{
    l_message(0,"usage: riposte [options]... [script [args]...]");
    l_message(0,"options:");
    l_message(0,"    -f, --file         execute R script");
    l_message(0,"    -v, --verbose      enable verbose output");
    l_message(0,"    -j N               launch Riposte with N threads");
}

extern int opterr;

int main(int argc, char** argv)
{
    /*  getopt parsing  */

    static struct option longopts[] = {
        { "debug",     0,    NULL,    'd' },
        { "file",      1,    NULL,    'f' },
        { "help",      0,    NULL,    'h' },
        { "verbose",   0,    NULL,    'v' },
        { "quiet",     0,    NULL,    'q' },
        { "script",    0,    NULL,    's' },
        { "args",      0,    NULL,    'a' },
        { "format",    1,    NULL,    'F' },
        { "profile",   1,    NULL,    'p' },
        { NULL,        0,    NULL,     0  }
    };

    /*  Parse commandline options  */
    char * filename = NULL;
    bool echo = true;
    char* profileName = NULL;
    Riposte::Format format = Riposte::RiposteFormat;
    int threads = 1; 

    int ch;
    opterr = 0;
    while ((ch = getopt_long(argc, argv, "df:hj:vqasp:", longopts, NULL)) != -1)
    {
        // don't parse args past '--args'
        if(ch == 'a')
            break;

        switch (ch) {
            case 'd':
                debug++;
                break;
            case 'f':
                filename = optarg;
                break;
            case 'v':
                verbose++;
                break;
            case 'q':
                echo = false;
                break;
            case 'j':
                if(0 != strcmp("-",optarg)) {
                    threads = atoi(optarg);
                }
                break;
            case 'F':
                if(0 == strcmp("R",optarg))
                    format = Riposte::RFormat;
                else
                    format = Riposte::RiposteFormat;
                break;
            case 'p':
                profileName = optarg;
                break;
            case 'h':
            default:
                usage();
                exit(-1);
                break;
        }
    }

    d_message(1,NULL,"Command option processing complete");

    if(!filename)
        info(threads, std::cout);

    /* Initialize Riposte VM */
    Riposte::initialize(argc, argv, threads, verbose, format, profileName!=NULL);

    /* Create an execution state for the main thread */
    Riposte::State& state = Riposte::newState();

    /* Load core functions */
    try {
        Environment* env = new Environment(1, global->empty);
        loadPackage((State&)state, env, "library", "core");
    } 
    catch(RiposteException const& e) { 
        e_message("Error", e.kind().c_str(), e.what().c_str());
    } 

    int rc; 
    /* Load bootstrap file if it exists */
    {
        std::ifstream in("bootstrap.R");
        rc = run((State&)state, std::string("bootstrap.R"), in, std::cout, false, echo);
    }

    /* Either execute the specified file or read interactively from stdin  */
    if(filename) {
        std::ifstream in(filename);
        rc = run((State&)state, std::string(filename), in, std::cout, false, echo);
    } 
    else {
        rc = run((State&)state, std::string("<stdin>"), std::cin, std::cout, true, echo);
    }

    /* Session over */

    fflush(stdout);
    fflush(stderr);

    if(profileName != NULL)
        global->dumpProfile(profileName);

    Riposte::deleteState(state);
    Riposte::finalize();

    return rc;
}

