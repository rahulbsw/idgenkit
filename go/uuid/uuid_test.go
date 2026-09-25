package uuid

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

func lines(t testing.TB, name string) []string {
	t.Helper()
	f, err := os.Open("../../testdata/" + name)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var out []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if line := sc.Text(); line != "" && !strings.HasPrefix(line, "#") {
			out = append(out, line)
		}
	}
	return out
}

func u64(t *testing.T, s string) uint64 {
	t.Helper()
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func TestVectors(t *testing.T) {
	rows := lines(t, "uuid.txt")
	for _, line := range rows {
		r := strings.Fields(line)
		var u UUID
		var err error
		var ms uint64
		switch r[0] {
		case "v4":
			var rnd [16]byte
			raw, _ := hex.DecodeString(r[1])
			copy(rnd[:], raw)
			u = V4FromRandom(rnd)
		case "v7":
			var rnd [10]byte
			raw, _ := hex.DecodeString(r[2])
			copy(rnd[:], raw)
			ms = u64(t, r[1])
			u, err = V7FromParts(ms, rnd)
		default:
			t.Fatalf("bad line %q", line)
		}
		want := r[len(r)-1]
		if want == "error" {
			if err != ErrTimeRange {
				t.Fatalf("%q: want ErrTimeRange, got %v", line, err)
			}
			continue
		}
		if err != nil || u.String() != want {
			t.Fatalf("%q: got %s, %v", line, u, err)
		}
		p, err := Parse(want)
		if err != nil || p != u || strconv.Itoa(p.Version()) != r[0][1:] {
			t.Fatalf("parse(%s) = %v, %v", want, p, err)
		}
		if upper, _ := Parse(strings.ToUpper(want)); upper != u {
			t.Fatalf("uppercase parse mismatch for %s", want)
		}
		ts, err := p.Time()
		if r[0] == "v7" && (err != nil || ts != ms) || r[0] == "v4" && err != ErrNotV7 {
			t.Fatalf("Time(%s) = %d, %v", want, ts, err)
		}
	}
	if len(rows) < 10 {
		t.Fatal("too few uuid vectors")
	}
}

func TestInvalid(t *testing.T) {
	rows := lines(t, "uuid_invalid.txt")
	for _, line := range rows {
		s := line[1 : len(line)-1]
		if _, err := Parse(s); err == nil {
			t.Fatalf("Parse(%q) succeeded", s)
		}
	}
	if len(rows) < 10 {
		t.Fatal("too few uuid_invalid vectors")
	}
}

func TestNew(t *testing.T) {
	seen := make(map[UUID]struct{}, 20000)
	for i := 0; i < 10000; i++ {
		for _, u := range []UUID{NewV4(), NewV7()} {
			if _, dup := seen[u]; dup {
				t.Fatal("duplicate UUID")
			}
			seen[u] = struct{}{}
			if u[8]&0xC0 != 0x80 {
				t.Fatalf("bad variant %s", u)
			}
		}
	}
	if NewV4().Version() != 4 || NewV7().Version() != 7 {
		t.Fatal("wrong version")
	}
	now := nowMs()
	if ts, err := NewV7().Time(); err != nil || ts < now || ts > now+5000 {
		t.Fatalf("NewV7 time %d, %v (now %d)", ts, err, now)
	}
}

func TestTextMarshal(t *testing.T) {
	u := NewV7()
	b, _ := u.MarshalText()
	var p UUID
	if err := p.UnmarshalText(b); err != nil || p != u {
		t.Fatalf("round trip %s: %v", b, err)
	}
	if err := p.UnmarshalText([]byte("nope")); err != ErrInvalidFormat {
		t.Fatalf("want ErrInvalidFormat, got %v", err)
	}
}

func TestMonotonic(t *testing.T) {
	m := NewMonotonic()
	ids := make([]string, 50000)
	prev := UUID{}
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

func TestMonotonicVectors(t *testing.T) {
	rows := lines(t, "uuid7_monotonic.txt")
	m := NewMonotonic()
	for _, line := range rows {
		r := strings.Fields(line)
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
		t.Fatal("too few uuid7_monotonic vectors")
	}
}

func TestMonotonicConcurrent(t *testing.T) {
	m := NewMonotonic()
	var mu sync.Mutex
	seen := map[UUID]struct{}{}
	var wg sync.WaitGroup
	for w := 0; w < 8; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			local := make([]UUID, 5000)
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

var sinkUUID UUID
var sinkString string

func BenchmarkNewV4(b *testing.B) {
	for i := 0; i < b.N; i++ {
		sinkUUID = NewV4()
	}
}

func BenchmarkNewV7(b *testing.B) {
	for i := 0; i < b.N; i++ {
		sinkUUID = NewV7()
	}
}

func BenchmarkNewV7String(b *testing.B) {
	for i := 0; i < b.N; i++ {
		sinkString = NewV7().String()
	}
}

func BenchmarkMonotonic(b *testing.B) {
	m := NewMonotonic()
	for i := 0; i < b.N; i++ {
		sinkUUID, _ = m.Next()
	}
}

func BenchmarkParse(b *testing.B) {
	s := NewV7().String()
	for i := 0; i < b.N; i++ {
		sinkUUID, _ = Parse(s)
	}
}
