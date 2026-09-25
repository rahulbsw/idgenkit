package ulid

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"os"
	"sort"
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

func TestVectors(t *testing.T) {
	for _, row := range vectors(t, "ulid.txt") {
		ms, _ := strconv.ParseUint(row[0], 10, 64)
		raw, _ := hex.DecodeString(row[1])
		var rnd [10]byte
		copy(rnd[:], raw)
		u, err := FromParts(ms, rnd)
		if err != nil {
			t.Fatal(err)
		}
		if got := u.String(); got != row[2] {
			t.Fatalf("encode(%s,%s) = %s, want %s", row[0], row[1], got, row[2])
		}
		p, err := Parse(row[2])
		if err != nil || p != u || p.Time() != ms || p.Random() != rnd {
			t.Fatalf("parse(%s) = %v, %v", row[2], p, err)
		}
		if lower, _ := Parse(strings.ToLower(row[2])); lower != u {
			t.Fatalf("lowercase parse mismatch for %s", row[2])
		}
	}
}

func TestInvalid(t *testing.T) {
	for _, row := range vectors(t, "ulid_invalid.txt") {
		if _, err := Parse(row[0]); err == nil {
			t.Fatalf("Parse(%q) succeeded", row[0])
		}
	}
	if _, err := Parse(""); err == nil {
		t.Fatal("empty string parsed")
	}
	if _, err := FromParts(MaxTime+1, [10]byte{}); err != ErrTimeRange {
		t.Fatal("expected ErrTimeRange")
	}
}

func TestNewUnique(t *testing.T) {
	seen := make(map[ULID]struct{}, 10000)
	for i := 0; i < 10000; i++ {
		u := New()
		if _, dup := seen[u]; dup {
			t.Fatal("duplicate ULID")
		}
		seen[u] = struct{}{}
	}
}

func TestMonotonic(t *testing.T) {
	m := NewMonotonic()
	ids := make([]string, 50000)
	prev := ULID{}
	for i := range ids {
		u, err := m.Next()
		if err != nil {
			t.Fatal(err)
		}
		if bytes.Compare(u[:], prev[:]) <= 0 {
			t.Fatalf("not increasing at %d", i)
		}
		prev = u
		ids[i] = u.String()
	}
	if !sort.StringsAreSorted(ids) {
		t.Fatal("string order differs from generation order")
	}
}

func TestMonotonicCarryAndOverflow(t *testing.T) {
	m := NewMonotonic()
	m.primed, m.lastMs = true, MaxTime
	putTime(&m.last, MaxTime)
	for i := 6; i < 16; i++ {
		m.last[i] = 0xFF
	}
	m.last[6] = 0x00
	u, err := m.Next()
	if err != nil || u[6] != 0x01 || u[15] != 0x00 {
		t.Fatalf("carry failed: %x %v", u, err)
	}
	for i := 6; i < 16; i++ {
		m.last[i] = 0xFF
	}
	for n := 0; n < 2; n++ {
		if _, err := m.Next(); err != ErrMonotonic {
			t.Fatalf("expected ErrMonotonic, got %v", err)
		}
	}
}

func TestMonotonicVectors(t *testing.T) {
	rows := vectors(t, "ulid_monotonic.txt")
	m := NewMonotonic()
	for _, r := range rows {
		if r[0] == "reset" {
			m = NewMonotonic()
			continue
		}
		now, want := u64(t, r[1]), r[3]
		drew := false
		fill := func(b []byte) {
			drew = true
			if r[2] != "-" {
				raw, _ := hex.DecodeString(r[2])
				copy(b, raw)
			}
		}
		u, err := m.next(now, fill)
		if r[2] == "-" && drew {
			t.Fatalf("%v: drew randomness", r)
		}
		if want == "error" {
			if err == nil {
				t.Fatalf("%v: got %s, want an error", r, u)
			}
		} else if err != nil || u.String() != want {
			t.Fatalf("%v: got %s, %v", r, u, err)
		}
	}
	if len(rows) < 10 {
		t.Fatal("too few ulid_monotonic vectors")
	}
}

func u64(t *testing.T, s string) uint64 {
	t.Helper()
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func TestMonotonicConcurrent(t *testing.T) {
	m := NewMonotonic()
	var mu sync.Mutex
	seen := map[ULID]struct{}{}
	var wg sync.WaitGroup
	for w := 0; w < 8; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			local := make([]ULID, 5000)
			for i := range local {
				local[i], _ = m.Next()
			}
			mu.Lock()
			for _, u := range local {
				seen[u] = struct{}{}
			}
			mu.Unlock()
		}()
	}
	wg.Wait()
	if len(seen) != 40000 {
		t.Fatalf("got %d unique of 40000", len(seen))
	}
}

var sinkULID ULID
var sinkString string

func BenchmarkNew(b *testing.B) {
	for i := 0; i < b.N; i++ {
		sinkULID = New()
	}
}

func BenchmarkNewString(b *testing.B) {
	for i := 0; i < b.N; i++ {
		sinkString = New().String()
	}
}

func BenchmarkMonotonic(b *testing.B) {
	m := NewMonotonic()
	for i := 0; i < b.N; i++ {
		sinkULID, _ = m.Next()
	}
}

func BenchmarkParse(b *testing.B) {
	s := New().String()
	for i := 0; i < b.N; i++ {
		sinkULID, _ = Parse(s)
	}
}

func BenchmarkNewParallel(b *testing.B) {
	b.RunParallel(func(pb *testing.PB) {
		var u ULID
		for pb.Next() {
			u = New()
		}
		_ = u
	})
}
