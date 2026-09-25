// Package snowflake generates time-ordered 64-bit IDs.
//
// Layout (compatible with github.com/dustinrouillard/snowflake-id):
//
//	| 42 bits: ms since epoch | 10 bits: machine id | 12 bits: sequence |
package snowflake

import (
	"errors"
	"runtime"
	"sync/atomic"
	"time"
)

const (
	TimestampBits = 42
	MachineIDBits = 10
	SequenceBits  = 12

	MaxMachineID      = 1<<MachineIDBits - 1
	MaxSequence       = 1<<SequenceBits - 1
	MaxTimestampDelta = 1<<TimestampBits - 1

	machineShift   = SequenceBits
	timestampShift = SequenceBits + MachineIDBits
)

var (
	ErrMachineID = errors.New("snowflake: machine id must be in [0, 1023]")
	ErrSequence  = errors.New("snowflake: sequence must be in [0, 4095]")
	ErrTimeRange = errors.New("snowflake: timestamp outside the 42-bit range for this epoch")
)

// Parts is a decoded Snowflake ID.
type Parts struct {
	TimestampMs uint64
	MachineID   uint16
	Sequence    uint16
}

// Compose builds an ID from its parts.
func Compose(timestampMs uint64, machineID, sequence uint16, epochMs uint64) (uint64, error) {
	if machineID > MaxMachineID {
		return 0, ErrMachineID
	}
	if sequence > MaxSequence {
		return 0, ErrSequence
	}
	if timestampMs < epochMs || timestampMs-epochMs > MaxTimestampDelta {
		return 0, ErrTimeRange
	}
	return (timestampMs-epochMs)<<timestampShift | uint64(machineID)<<machineShift | uint64(sequence), nil
}

// Parse splits an ID into its parts.
func Parse(id, epochMs uint64) Parts {
	return Parts{
		TimestampMs: id>>timestampShift + epochMs,
		MachineID:   uint16(id >> machineShift & MaxMachineID),
		Sequence:    uint16(id & MaxSequence),
	}
}

// Generator is a lock-free Snowflake generator safe for concurrent use.
//
// After 4096 IDs in one millisecond it waits for the next millisecond. If the
// wall clock moves backwards it keeps issuing from the last observed
// millisecond, so IDs never decrease.
type Generator struct {
	machineID uint64
	epochMs   uint64
	// state packs (lastMs << SequenceBits | sequence) for a single CAS.
	state atomic.Uint64
}

// New returns a generator for the given machine id and custom epoch (ms since
// the Unix epoch; 0 matches the reference implementation's default).
func New(machineID uint16, epochMs uint64) (*Generator, error) {
	if machineID > MaxMachineID {
		return nil, ErrMachineID
	}
	return &Generator{machineID: uint64(machineID), epochMs: epochMs}, nil
}

func nowMs() uint64 { return uint64(time.Now().UnixMilli()) }

// Next returns a new unique ID.
func (g *Generator) Next() (uint64, error) { return g.next(nowMs) }

func (g *Generator) next(clock func() uint64) (uint64, error) {
	for {
		old := g.state.Load()
		lastMs, seq := old>>SequenceBits, old&MaxSequence
		now := clock()
		var ms, next uint64
		switch {
		case now > lastMs:
			ms, next = now, 0
		case seq < MaxSequence:
			ms, next = lastMs, seq+1
		default:
			for clock() <= lastMs {
				runtime.Gosched()
			}
			continue
		}
		if ms < g.epochMs || ms-g.epochMs > MaxTimestampDelta {
			return 0, ErrTimeRange
		}
		if g.state.CompareAndSwap(old, ms<<SequenceBits|next) {
			return (ms-g.epochMs)<<timestampShift | g.machineID<<machineShift | next, nil
		}
	}
}

// Parse decodes an ID produced by this generator.
func (g *Generator) Parse(id uint64) Parts { return Parse(id, g.epochMs) }
