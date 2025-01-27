package tefmocheck

import (
	"fmt"
	"path"
)

// taskStateCheck checks if the swarming task is in State.
type taskStateCheck struct {
	State           string
	swarmingSummary *SwarmingTaskSummary
}

func (c *taskStateCheck) Check(to *TestingOutputs) bool {
	c.swarmingSummary = to.SwarmingSummary
	return to.SwarmingSummary.Results.State == c.State
}

func (c *taskStateCheck) Name() string {
	return path.Join("task_state", c.State)
}

func (c *taskStateCheck) DebugText() string {
	return debugTextForSwarmingSummary(c.swarmingSummary)
}

// taskFailureCheck checks if the swarming task failed.
type taskFailureCheck struct {
	swarmingSummary *SwarmingTaskSummary
}

func (c *taskFailureCheck) Check(to *TestingOutputs) bool {
	c.swarmingSummary = to.SwarmingSummary
	return to.SwarmingSummary.Results.State == "COMPLETED" && to.SwarmingSummary.Results.Failure
}

func (c *taskFailureCheck) Name() string {
	return "task_failure"
}

func (c *taskFailureCheck) DebugText() string {
	return debugTextForSwarmingSummary(c.swarmingSummary)
}

// taskInternalFailureCheck checks if the swarming task internally failed.
type taskInternalFailureCheck struct {
	swarmingSummary *SwarmingTaskSummary
}

func (c *taskInternalFailureCheck) Check(to *TestingOutputs) bool {
	c.swarmingSummary = to.SwarmingSummary
	return to.SwarmingSummary.Results.State == "COMPLETED" && to.SwarmingSummary.Results.InternalFailure
}

func (c *taskInternalFailureCheck) Name() string {
	return "task_internal_failure"
}

func (c *taskInternalFailureCheck) DebugText() string {
	return debugTextForSwarmingSummary(c.swarmingSummary)
}

func debugTextForSwarmingSummary(swarmingSummary *SwarmingTaskSummary) string {
	ret := fmt.Sprintf("Swarming task state: %s.", swarmingSummary.Results.State)
	if swarmingSummary.Results.Failure {
		ret += fmt.Sprintf("\nTask failure.")
	}
	if swarmingSummary.Results.InternalFailure {
		ret += fmt.Sprintf("\nTask internal failure.")
	}
	return fmt.Sprintf(`%s
The task's log is in %s.
The task ID is %s.
The task ran on bot %s.`,
		ret, swarmingOutputType, swarmingSummary.Results.TaskId, swarmingSummary.Results.BotId)
}

// TaskStateChecks contains checks to cover every possible state.
// A task can only be in one state, so their relative order doesn't matter.
var TaskStateChecks []FailureModeCheck = []FailureModeCheck{
	// Covers state == COMPLETED.
	&taskInternalFailureCheck{},
	&taskFailureCheck{},
	// All other states.
	&taskStateCheck{State: "BOT_DIED"},
	&taskStateCheck{State: "CANCELED"},
	&taskStateCheck{State: "EXPIRED"},
	&taskStateCheck{State: "INVALID"},
	&taskStateCheck{State: "KILLED"},
	&taskStateCheck{State: "NO_RESOURCE"},
	&taskStateCheck{State: "PENDING"},
	&taskStateCheck{State: "RUNNING"},
	&taskStateCheck{State: "TIMED_OUT"},
}
