ifdef JAVA_HOME
JAVAC := $(JAVA_HOME)/bin/javac
JAVA  := $(JAVA_HOME)/bin/java
JAR   := $(JAVA_HOME)/bin/jar
else
JAVAC := javac
JAVA  := java
JAR   := jar
endif
BUILD := build
SRC   := $(shell find src/main/java -name '*.java')

.PHONY: all test bench jar clean

all: $(BUILD)/classes

$(BUILD)/classes: $(SRC)
	@mkdir -p $@
	$(JAVAC) --release 17 -Xlint:all -Werror -d $@ $(SRC)
	@touch $@

$(BUILD)/test: $(BUILD)/classes $(shell find test -name '*.java')
	@mkdir -p $@
	$(JAVAC) --release 17 -cp $(BUILD)/classes -d $@ $$(find test -name '*.java')
	@touch $@

$(BUILD)/bench: $(BUILD)/classes $(shell find bench -name '*.java')
	@mkdir -p $@
	$(JAVAC) --release 17 -cp $(BUILD)/classes -d $@ $$(find bench -name '*.java')
	@touch $@

test: $(BUILD)/test
	$(JAVA) -ea -cp $(BUILD)/classes:$(BUILD)/test io.github.rahulbsw.idgenkit.Conformance ../testdata

bench: $(BUILD)/bench
	$(JAVA) -cp $(BUILD)/classes:$(BUILD)/bench io.github.rahulbsw.idgenkit.Bench

jar: $(BUILD)/classes
	$(JAR) --create --file $(BUILD)/idgenkit-0.1.0.jar -C $(BUILD)/classes .

clean:
	rm -rf $(BUILD) target
