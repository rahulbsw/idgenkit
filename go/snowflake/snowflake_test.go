package snowflake

import (
	"bufio"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"
)

func vectors(t testing.TB, name string) [][]string {
	t.Helper()
	f, err := os.Open("../../testdata/" + name)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var rows [][]string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := sc.Text()
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		rows = append(rows, strings.Fields(line))
	}
	return rows
}

func u64(s string) uint64 {
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		panic(err)
	}
	return v
}

func TestVectors(t *testing.T) {
	for _, r := range vectors(t, "snowflake.txt") {
		epoch, mid, seq, ts, id := u64(r[0]), uint16(u64(r[1])), uint16(u64(r[2])), u64(r[3]), u64(r[4])
		got, err := Compose(ts, mid, seq, epoch)
		if err != nil || got != id {
			t.Fatalf("Compose%v = %d, %v; want %d", r, got, err, id)
		}
		if p := Parse(id, epoch); p != (Parts{ts, mid, seq}) {
			t.Fatalf("Parse(%d) = %+v", id, p)
		}
	}
}

func scriptedClock(readings []uint64) func() uint64 {
	return func() uint64 {
		v := readings[0]
		if len(readings) > 1 {
			readings = readings[1:]
		}
		return v
	}
}

func TestSequenceVectors(t *testing.T) {
	rows := vectors(t, "snowflake_sequence.txt")
	var g *Generator
	var prev uint64
	for _, r := range rows {
		switch r[0] {
		case "gen":
			g, _ = New(uint16(u64(r[1])), u64(r[2]))
			prev = 0
		case "fill":
			clock := func() uint64 { return u64(r[2]) }
			for i := u64(r[1]); i > 0; i-- {
				id, err := g.next(clock)
				if err != nil || id <= prev {
					t.Fatalf("%v: got %d, %v after %d", r, id, err, prev)
				}
				prev = id
			}
		case "next":
			var readings []uint64
			for _, s := range strings.Split(r[1], ",") {
				readings = append(readings, u64(s))
			}
			id, err := g.next(scriptedClock(readings))
			if r[2] == "error" {
				if err == nil {
					t.Fatalf("%v: got %d, want an error", r, id)
				}
			} else if err != nil || id != u64(r[2]) {
				t.Fatalf("%v: got %d, %v", r, id, err)
			} else {
				prev = id
			}
		default:
			t.Fatalf("bad line %v", r)
		}
	}
	if len(rows) < 10 {
		t.Fatal("too few snowflake_sequence vectors")
	}
}

func TestValidation(t *testing.T) {
	if _, err := New(1024, 0); err != ErrMachineID {
		t.Fatal("expected ErrMachineID")
	}
	if _, err := Compose(0, 0, 4096, 0); err != ErrSequence {
		t.Fatal("expected ErrSequence")
	}
	if _, err := Compose(5, 0, 0, 10); err != ErrTimeRange {
		t.Fatal("expected ErrTimeRange")
	}
	g, _ := New(1, nowMs()+1_000_000)
	if _, err := g.Next(); err != ErrTimeRange {
		t.Fatal("expected ErrTimeRange for future epoch")
	}
}

func TestOrderedUnique(t *testing.T) {
	g, _ := New(7, 1_600_000_000_000)
	var prev uint64
	for i := 0; i < 100000; i++ {
		id, err := g.Next()
		if err != nil {
			t.Fatal(err)
		}
		if id <= prev {
			t.Fatalf("not increasing: %d after %d", id, prev)
		}
		prev = id
	}
	if p := g.Parse(prev); p.MachineID != 7 || p.TimestampMs < 1_600_000_000_000 {
		t.Fatalf("bad parts %+v", p)
	}
}

func TestClockBackwards(t *testing.T) {
	g, _ := New(1, 0)
	first, _ := g.Next()
	future := nowMs() + 5
	g.state.Store(future << SequenceBits)
	second, _ := g.Next()
	if second <= first || Parse(second, 0).TimestampMs != future {
		t.Fatalf("clock regression not handled: %d -> %d", first, second)
	}
}

func TestConcurrent(t *testing.T) {
	g, _ := New(3, 0)
	var mu sync.Mutex
	seen := map[uint64]struct{}{}
	var wg sync.WaitGroup
	for w := 0; w < 8; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			local := make([]uint64, 20000)
			for i := range local {
				local[i], _ = g.Next()
			}
			mu.Lock()
			for _, id := range local {
				seen[id] = struct{}{}
			}
			mu.Unlock()
		}()
	}
	wg.Wait()
	if len(seen) != 160000 {
		t.Fatalf("got %d unique of 160000", len(seen))
	}
}

var sink uint64

func BenchmarkNext(b *testing.B) {
	g, _ := New(1, 0)
	for i := 0; i < b.N; i++ {
		sink, _ = g.Next()
	}
}

func BenchmarkNextParallel(b *testing.B) {
	g, _ := New(1, 0)
	b.RunParallel(func(pb *testing.PB) {
		var id uint64
		for pb.Next() {
			id, _ = g.Next()
		}
		_ = id
	})
}
