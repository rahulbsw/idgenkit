package relid

import (
	"bufio"
	"encoding/hex"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

var testSecret = []byte("test-only-secret-0123456789")

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

func unhex(t *testing.T, s string) []byte {
	t.Helper()
	if s == "-" {
		return nil
	}
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestTagVectors(t *testing.T) {
	rows := lines(t, "relid_tag.txt")
	if len(rows) < 10 {
		t.Fatal("too few vectors")
	}
	for _, line := range rows {
		r := strings.Fields(line)
		g, err := New(unhex(t, r[0]), string(unhex(t, r[1])))
		if err != nil {
			t.Fatal(err)
		}
		key := string(unhex(t, r[2]))
		if got := g.TagValue(key); uint64(got) != u64(t, r[3]) {
			t.Errorf("%s: tag %d", line, got)
		}
		if got := g.Tag(key); got != r[4] {
			t.Errorf("%s: tag text %s", line, got)
		}
	}
	if _, err := New([]byte("0123456789abcde"), ""); err != ErrSecret {
		t.Errorf("short secret: %v", err)
	}
}

func TestVectors(t *testing.T) {
	rows := lines(t, "relid.txt")
	for _, line := range rows {
		r := strings.Fields(line)
		switch r[0] {
		case "parts":
			tag, ms, rnd := u64(t, r[1]), u64(t, r[2]), u64(t, r[3])
			got, err := FromParts(uint32(tag), ms, rnd)
			if tag > 0xFFFFFFFF || r[4] == "error" {
				if err == nil {
					t.Errorf("%s: no error", line)
				}
				continue
			}
			if err != nil || got != r[4] {
				t.Errorf("%s: got %s %v", line, got, err)
			}
			if p, err := Parse(r[4]); err != nil || p != (Parts{uint32(tag), ms, rnd}) {
				t.Errorf("%s: parse %+v %v", line, p, err)
			}
		case "parse":
			want := Parts{uint32(u64(t, r[2])), u64(t, r[3]), u64(t, r[4])}
			if p, err := Parse(r[1]); err != nil || p != want {
				t.Errorf("%s: parse %+v %v", line, p, err)
			}
		default:
			t.Fatalf("bad line %s", line)
		}
	}
}

func TestInvalid(t *testing.T) {
	rows := lines(t, "relid_invalid.txt")
	if len(rows) < 10 {
		t.Fatal("too few vectors")
	}
	for _, line := range rows {
		s := line[1 : len(line)-1]
		if _, err := Parse(s); err == nil {
			t.Errorf("accepted %q", s)
		}
	}
}

func TestMonotonicVectors(t *testing.T) {
	g, _ := New(testSecret, "")
	for _, line := range lines(t, "relid_monotonic.txt") {
		r := strings.Fields(line)
		if r[0] == "reset" {
			g, _ = New(testSecret, "")
			continue
		}
		tag, now, rnd, want := uint32(u64(t, r[1])), u64(t, r[2]), r[3], r[4]
		drew := false
		fill := func(b []byte) {
			drew = true
			if rnd != "-" {
				copy(b, unhex(t, rnd))
			}
		}
		got, err := g.monotonicAt(tag, now, fill)
		if want == "error" {
			if err == nil {
				t.Errorf("%s: no error, got %s", line, got)
			}
		} else if err != nil || got != want {
			t.Errorf("%s: got %s %v", line, got, err)
		}
		if rnd == "-" && drew {
			t.Errorf("%s: drew randomness", line)
		}
	}
}

func TestGenerate(t *testing.T) {
	orders, _ := New(testSecret, "orders")
	invoices, _ := New(testSecret, "invoices")
	now := uint64(time.Now().UnixMilli())
	tag := orders.Tag("customer-42")
	seen := map[string]bool{}
	for i := 0; i < 10000; i++ {
		id, err := orders.Generate("customer-42")
		if err != nil || !strings.HasPrefix(id, tag+"-") || seen[id] {
			t.Fatalf("id %s %v", id, err)
		}
		seen[id] = true
	}
	if invoices.Tag("customer-42") == tag {
		t.Error("salt did not change the tag")
	}
	id, _ := orders.Generate("customer-42")
	p, err := Parse(id)
	if err != nil || p.TagText() != tag || p.Time < now || p.Time > now+5000 {
		t.Errorf("parse %s: %+v %v", id, p, err)
	}
}

func TestMonotonic(t *testing.T) {
	g, _ := New(testSecret, "")
	last := map[string]string{}
	for i := 0; i < 30000; i++ {
		key := "customer-42"
		if i%3 == 0 {
			key = "customer-7"
		}
		id, err := g.Monotonic(key)
		if err != nil || id <= last[key] {
			t.Fatalf("%s after %s: %v", id, last[key], err)
		}
		last[key] = id
	}
	var mu sync.Mutex
	var wg sync.WaitGroup
	seen := map[string]bool{}
	for w := 0; w < 8; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 5000; i++ {
				id, _ := g.Monotonic("k")
				mu.Lock()
				seen[id] = true
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if len(seen) != 40000 {
		t.Errorf("%d unique of 40000", len(seen))
	}
}

func BenchmarkGenerate(b *testing.B) {
	g, _ := New(testSecret, "orders")
	for i := 0; i < b.N; i++ {
		_, _ = g.Generate("customer-42")
	}
}

func BenchmarkMonotonic(b *testing.B) {
	g, _ := New(testSecret, "orders")
	for i := 0; i < b.N; i++ {
		_, _ = g.Monotonic("customer-42")
	}
}
