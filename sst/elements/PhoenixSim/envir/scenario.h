//==========================================================================
//  SCENARIO.H - part of
//                     OMNeT++/OMNEST
//            Discrete System Simulation in C++
//
//  Author: Andras Varga
//
//==========================================================================

/*--------------------------------------------------------------*
  Copyright (C) 1992-2008 Andras Varga
  Copyright (C) 2006-2008 OpenSim Ltd.

  This file is distributed WITHOUT ANY WARRANTY. See the file
  `license' for details on this and other legal matters.
*--------------------------------------------------------------*/

#ifndef SCENARIO_H_
#define SCENARIO_H_

#include <map>
#include <vector>
#include <set>
#include <string>
#include "envirdefs.h"
#include "common/expression.h"
#include "valueiterator.h"
#include "omnetconfiguration.h"

/**
 * Helper class for OmnetConfiguration: Resolves iterations in a scenario.
 * May be used as an iterator as well.
 */
class Scenario
{
  public:
    typedef OmnetConfiguration::IterationVariable IterationVariable;

  private:
    // the input
    const std::vector<IterationVariable>& vars;
    Expression *constraint;

    // the iteration variables, named (${x=1..5}) and unnamed (${1..5})
    std::vector<ValueIterator> variterators; // indices correspond to vars[]
    std::map<std::string, ValueIterator*> varmap; // varid-to-iterator

  private:
    bool inc();
    bool evaluateConstraint();
    Expression::Value getIterationVariable(const char *varname);

  public:
    /**
     * Used in resolving and generating scenarios, it implements a $x iteration variable.
     * Currently just delegates to OmnetConfiguration::getIterationVariable().
     */
    class VariableReference : public Expression::Variable
    {
      private:
        Scenario *hostnode;
        std::string varname;
      public:
        VariableReference(Scenario *node, const char *name) {hostnode = node; varname = name;}
        virtual ~VariableReference() {}
        virtual Expression::Functor *dup() const {return new VariableReference(hostnode, varname.c_str());}
        virtual const char *getName() const {return varname.c_str();}
        virtual char getReturnType() const {return Expression::Value::DBL;}
        virtual Expression::Value evaluate(Expression::Value args[], int numargs)
            {return hostnode->getIterationVariable(varname.c_str());}
    };

  public:
    Scenario(const std::vector<IterationVariable>& iterationVariables, const char *constraint);
    ~Scenario();

    /**
     * Returns the constructor parameter, unchanged.
     */
    const std::vector<IterationVariable>& getIterationVariables() const {return vars;}

    /**
     * Counts the number of runs this scenario generates.
     *
     * The current iteration state is NOT preserved.
     */
    int getNumRuns();

    /**
     * Spins the iteration variables to the given run.
     */
    void gotoRun(int runNumber);

    /**
     * Restarts the iteration. Returns false if there is no valid config at all,
     * that is, get() and next() may not be called.
     */
    bool restart();

    /**
     * Advances to the next valid run. When it returns false, that means the
     * the iteration finished, and get() and next() may not be invoked any more.
     */
    bool next();

    /**
     * XXX
     */
    std::string getVariable(const char *varid) const;

    /**
     * XXX
     */
    int getIteratorPosition(const char *varid) const;

    /**
     * Returns the current iteration state as a string ("$x=100, $y=3, $2=.4").
     * Used for the value of ${iterationvars} as well.
     */
    std::string str() const;
};

#endif


