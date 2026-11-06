#include "st_bootstrap.h"
#include "st_compiler.h"
#include <string.h>

struct BootstrapMethodDef {
  uint16_t    clsId;
  bool        isClassMethod;
  const char* source;
};

static const BootstrapMethodDef kBootstrapMethods[] = {
  // =========================================================================
  // Object
  // =========================================================================
  { CLS_OBJECT, false, "== anObject\n  <primitive: 30>" },
  { CLS_OBJECT, false, "~~ anObject\n  ^ (self == anObject) not" },
  { CLS_OBJECT, false, "= anObject\n  <primitive: 7>\n  ^ self == anObject" },
  { CLS_OBJECT, false, "~= anObject\n  <primitive: 8>\n  ^ (self = anObject) not" },
  { CLS_OBJECT, false, "class\n  <primitive: 31>" },
  { CLS_OBJECT, false, "yourself\n  ^ self" },
  { CLS_OBJECT, false, "isNil\n  ^ false" },
  { CLS_OBJECT, false, "notNil\n  ^ true" },
  { CLS_OBJECT, false, "ifNil: nilBlock\n  ^ self" },
  { CLS_OBJECT, false, "ifNotNil: aBlock\n  ^ aBlock value" },
  { CLS_OBJECT, false, "ifNil: nilBlock ifNotNil: aBlock\n  ^ aBlock value" },
  { CLS_OBJECT, false, "copy\n  <primitive: 37>" },
  { CLS_OBJECT, false, "shallowCopy\n  <primitive: 37>" },
  { CLS_OBJECT, false, "basicSize\n  <primitive: 32>" },
  { CLS_OBJECT, false, "size\n  <primitive: 32>" },
  { CLS_OBJECT, false, "basicAt: index\n  <primitive: 33>" },
  { CLS_OBJECT, false, "basicAt: index put: val\n  <primitive: 34>" },
  { CLS_OBJECT, false, "instVarAt: index\n  <primitive: 35>" },
  { CLS_OBJECT, false, "instVarAt: index put: val\n  <primitive: 36>" },
  { CLS_OBJECT, false, "perform: aSelector\n  <primitive: 38>" },
  { CLS_OBJECT, false, "perform: aSelector with: anObject\n  <primitive: 39>" },
  { CLS_OBJECT, false, "printString\n  <primitive: 41>" },
  { CLS_OBJECT, false, "asString\n  ^ self printString" },
  { CLS_OBJECT, false, "printOn: aStream\n  aStream nextPutAll: self printString" },
  { CLS_OBJECT, false, "error: aString\n  <primitive: 42>" },
  { CLS_OBJECT, false, "isMemberOf: aClass\n  ^ self class == aClass" },
  { CLS_OBJECT, false, "isKindOf: aClass\n  | c |\n  c := self class.\n  [ c notNil ] whileTrue: [\n    c == aClass ifTrue: [ ^ true ].\n    c := c superclass\n  ].\n  ^ false" },
  { CLS_OBJECT, false, "respondsTo: aSymbol\n  ^ self class canUnderstand: aSymbol" },

  // =========================================================================
  // UndefinedObject (nil)
  // =========================================================================
  { CLS_UNDEFINED_OBJECT, false, "isNil\n  ^ true" },
  { CLS_UNDEFINED_OBJECT, false, "notNil\n  ^ false" },
  { CLS_UNDEFINED_OBJECT, false, "ifNil: nilBlock\n  ^ nilBlock value" },
  { CLS_UNDEFINED_OBJECT, false, "ifNotNil: aBlock\n  ^ nil" },
  { CLS_UNDEFINED_OBJECT, false, "ifNil: nilBlock ifNotNil: aBlock\n  ^ nilBlock value" },
  { CLS_UNDEFINED_OBJECT, false, "printString\n  ^ 'nil'" },

  // =========================================================================
  // Boolean, True, False
  // =========================================================================
  { CLS_TRUE,  false, "ifTrue: trueBlock\n  ^ trueBlock value" },
  { CLS_TRUE,  false, "ifFalse: falseBlock\n  ^ nil" },
  { CLS_TRUE,  false, "ifTrue: trueBlock ifFalse: falseBlock\n  ^ trueBlock value" },
  { CLS_TRUE,  false, "ifFalse: falseBlock ifTrue: trueBlock\n  ^ trueBlock value" },
  { CLS_TRUE,  false, "not\n  ^ false" },
  { CLS_TRUE,  false, "& aBoolean\n  ^ aBoolean" },
  { CLS_TRUE,  false, "| aBoolean\n  ^ true" },
  { CLS_TRUE,  false, "and: aBlock\n  ^ aBlock value" },
  { CLS_TRUE,  false, "or: aBlock\n  ^ true" },
  { CLS_TRUE,  false, "xor: aBoolean\n  ^ aBoolean not" },
  { CLS_TRUE,  false, "eqv: aBoolean\n  ^ aBoolean" },

  { CLS_FALSE, false, "ifTrue: trueBlock\n  ^ nil" },
  { CLS_FALSE, false, "ifFalse: falseBlock\n  ^ falseBlock value" },
  { CLS_FALSE, false, "ifTrue: trueBlock ifFalse: falseBlock\n  ^ falseBlock value" },
  { CLS_FALSE, false, "ifFalse: falseBlock ifTrue: trueBlock\n  ^ falseBlock value" },
  { CLS_FALSE, false, "not\n  ^ true" },
  { CLS_FALSE, false, "& aBoolean\n  ^ false" },
  { CLS_FALSE, false, "| aBoolean\n  ^ aBoolean" },
  { CLS_FALSE, false, "and: aBlock\n  ^ false" },
  { CLS_FALSE, false, "or: aBlock\n  ^ aBlock value" },
  { CLS_FALSE, false, "xor: aBoolean\n  ^ aBoolean" },
  { CLS_FALSE, false, "eqv: aBoolean\n  ^ aBoolean not" },

  // =========================================================================
  // Magnitude & Number
  // =========================================================================
  { CLS_MAGNITUDE, false, "< aMagnitude\n  <primitive: 3>" },
  { CLS_MAGNITUDE, false, "> aMagnitude\n  <primitive: 4>\n  ^ aMagnitude < self" },
  { CLS_MAGNITUDE, false, "<= aMagnitude\n  <primitive: 5>\n  ^ (self > aMagnitude) not" },
  { CLS_MAGNITUDE, false, ">= aMagnitude\n  <primitive: 6>\n  ^ (self < aMagnitude) not" },
  { CLS_MAGNITUDE, false, "between: min and: max\n  ^ (self >= min) and: [ self <= max ]" },
  { CLS_MAGNITUDE, false, "min: aMagnitude\n  ^ self < aMagnitude ifTrue: [ self ] ifFalse: [ aMagnitude ]" },
  { CLS_MAGNITUDE, false, "max: aMagnitude\n  ^ self > aMagnitude ifTrue: [ self ] ifFalse: [ aMagnitude ]" },

  { CLS_NUMBER, false, "+ aNumber\n  <primitive: 1>" },
  { CLS_NUMBER, false, "- aNumber\n  <primitive: 2>" },
  { CLS_NUMBER, false, "* aNumber\n  <primitive: 9>" },
  { CLS_NUMBER, false, "/ aNumber\n  <primitive: 10>" },
  { CLS_NUMBER, false, "\\\\ aNumber\n  <primitive: 11>" },
  { CLS_NUMBER, false, "// aNumber\n  <primitive: 12>" },
  { CLS_NUMBER, false, "quo: aNumber\n  <primitive: 13>" },
  { CLS_NUMBER, false, "rem: aNumber\n  ^ self - ((self quo: aNumber) * aNumber)" },
  { CLS_NUMBER, false, "abs\n  ^ self < 0 ifTrue: [ 0 - self ] ifFalse: [ self ]" },
  { CLS_NUMBER, false, "negated\n  ^ 0 - self" },
  { CLS_NUMBER, false, "squared\n  ^ self * self" },
  { CLS_NUMBER, false, "even\n  ^ (self \\\\ 2) = 0" },
  { CLS_NUMBER, false, "odd\n  ^ (self \\\\ 2) ~= 0" },
  { CLS_NUMBER, false, "sign\n  self > 0 ifTrue: [ ^ 1 ].\n  self < 0 ifTrue: [ ^ -1 ].\n  ^ 0" },
  { CLS_NUMBER, false, "sin\n  <primitive: 20>" },
  { CLS_NUMBER, false, "cos\n  <primitive: 21>" },
  { CLS_NUMBER, false, "sqrt\n  <primitive: 22>" },
  { CLS_NUMBER, false, "asFloat\n  <primitive: 18>" },
  { CLS_NUMBER, false, "asInteger\n  <primitive: 19>" },
  { CLS_NUMBER, false, "truncated\n  <primitive: 19>" },
  { CLS_NUMBER, false, "rounded\n  <primitive: 23>" },
  { CLS_NUMBER, false, "floor\n  <primitive: 24>" },
  { CLS_NUMBER, false, "ceiling\n  <primitive: 25>" },
  { CLS_NUMBER, false, "@ y\n  ^ Point x: self y: y" },
  { CLS_NUMBER, false, "to: stop do: aBlock\n  | i |\n  i := self.\n  [ i <= stop ] whileTrue: [\n    aBlock value: i.\n    i := i + 1\n  ]" },
  { CLS_NUMBER, false, "to: stop by: step do: aBlock\n  | i |\n  i := self.\n  step > 0\n    ifTrue: [ [ i <= stop ] whileTrue: [ aBlock value: i. i := i + step ] ]\n    ifFalse: [ [ i >= stop ] whileTrue: [ aBlock value: i. i := i + step ] ]" },
  { CLS_NUMBER, false, "timesRepeat: aBlock\n  | i |\n  i := 1.\n  [ i <= self ] whileTrue: [\n    aBlock value.\n    i := i + 1\n  ]" },

  // =========================================================================
  // SmallInteger
  // =========================================================================
  { CLS_SMALL_INTEGER, false, "bitAnd: anInt\n  <primitive: 14>" },
  { CLS_SMALL_INTEGER, false, "bitOr: anInt\n  <primitive: 15>" },
  { CLS_SMALL_INTEGER, false, "bitXor: anInt\n  <primitive: 16>" },
  { CLS_SMALL_INTEGER, false, "bitShift: anInt\n  <primitive: 17>" },
  { CLS_SMALL_INTEGER, false, "factorial\n  | acc i |\n  acc := 1.\n  i := 2.\n  [ i <= self ] whileTrue: [\n    acc := acc * i.\n    i := i + 1\n  ].\n  ^ acc" },
  { CLS_SMALL_INTEGER, false, "gcd: anInt\n  | a b t |\n  a := self abs.\n  b := anInt abs.\n  [ b ~= 0 ] whileTrue: [\n    t := a \\\\ b.\n    a := b.\n    b := t\n  ].\n  ^ a" },
  { CLS_SMALL_INTEGER, false, "isPrime\n  | d |\n  self < 2 ifTrue: [ ^ false ].\n  d := 2.\n  [ (d * d) <= self ] whileTrue: [\n    (self \\\\ d) = 0 ifTrue: [ ^ false ].\n    d := d + 1\n  ].\n  ^ true" },

  // =========================================================================
  // Character
  // =========================================================================
  { CLS_CHARACTER, false, "value\n  <primitive: 65>" },
  { CLS_CHARACTER, false, "asciiValue\n  <primitive: 65>" },
  { CLS_CHARACTER, false, "asInteger\n  <primitive: 65>" },
  { CLS_CHARACTER, false, "asString\n  | s |\n  s := String new: 1.\n  s at: 1 put: self.\n  ^ s" },
  { CLS_CHARACTER, false, "isDigit\n  | v |\n  v := self value.\n  ^ (v >= 48) and: [ v <= 57 ]" },
  { CLS_CHARACTER, false, "isUppercase\n  | v |\n  v := self value.\n  ^ (v >= 65) and: [ v <= 90 ]" },
  { CLS_CHARACTER, false, "isLowercase\n  | v |\n  v := self value.\n  ^ (v >= 97) and: [ v <= 122 ]" },
  { CLS_CHARACTER, false, "isLetter\n  ^ self isUppercase or: [ self isLowercase ]" },
  { CLS_CHARACTER, false, "isSeparator\n  | v |\n  v := self value.\n  ^ (v = 32) or: [ (v = 9) or: [ (v = 10) or: [ v = 13 ] ] ]" },
  { CLS_CHARACTER, false, "asUppercase\n  ^ self isLowercase ifTrue: [ Character value: (self value - 32) ] ifFalse: [ self ]" },
  { CLS_CHARACTER, false, "asLowercase\n  ^ self isUppercase ifTrue: [ Character value: (self value + 32) ] ifFalse: [ self ]" },
  { CLS_CHARACTER, true,  "value: anInteger\n  <primitive: 66>" },
  { CLS_CHARACTER, true,  "cr\n  <primitive: 66>\n  ^ self value: 10" },
  { CLS_CHARACTER, true,  "space\n  ^ self value: 32" },
  { CLS_CHARACTER, true,  "tab\n  ^ self value: 9" },

  // =========================================================================
  // Collection & SequenceableCollection
  // =========================================================================
  { CLS_COLLECTION, false, "isEmpty\n  ^ self size = 0" },
  { CLS_COLLECTION, false, "notEmpty\n  ^ self size > 0" },
  { CLS_COLLECTION, false, "includes: anObject\n  self do: [ :each | each = anObject ifTrue: [ ^ true ] ].\n  ^ false" },
  { CLS_COLLECTION, false, "detect: aBlock ifNone: exceptionBlock\n  self do: [ :each | (aBlock value: each) ifTrue: [ ^ each ] ].\n  ^ exceptionBlock value" },
  { CLS_COLLECTION, false, "detect: aBlock\n  ^ self detect: aBlock ifNone: [ nil ]" },
  { CLS_COLLECTION, false, "inject: thisValue into: binaryBlock\n  | acc |\n  acc := thisValue.\n  self do: [ :each | acc := binaryBlock value: acc value: each ].\n  ^ acc" },
  { CLS_COLLECTION, false, "select: aBlock\n  | oc |\n  oc := OrderedCollection new.\n  self do: [ :each | (aBlock value: each) ifTrue: [ oc add: each ] ].\n  ^ oc asArray" },
  { CLS_COLLECTION, false, "reject: aBlock\n  | oc |\n  oc := OrderedCollection new.\n  self do: [ :each | (aBlock value: each) ifFalse: [ oc add: each ] ].\n  ^ oc asArray" },

  { CLS_SEQUENCEABLE_COLLECTION, false, "at: index\n  <primitive: 33>" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "at: index put: val\n  <primitive: 34>" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "first\n  ^ self at: 1" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "second\n  ^ self at: 2" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "last\n  ^ self at: self size" },
  { CLS_SEQUENCEABLE_COLLECTION, false, ", aCollection\n  <primitive: 62>" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "copyFrom: start to: stop\n  <primitive: 63>" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "do: aBlock\n  | i n |\n  i := 1.\n  n := self size.\n  [ i <= n ] whileTrue: [\n    aBlock value: (self at: i).\n    i := i + 1\n  ]" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "withIndexDo: aBlock\n  | i n |\n  i := 1.\n  n := self size.\n  [ i <= n ] whileTrue: [\n    aBlock value: (self at: i) value: i.\n    i := i + 1\n  ]" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "collect: aBlock\n  | res i n |\n  n := self size.\n  res := Array new: n.\n  i := 1.\n  [ i <= n ] whileTrue: [\n    res at: i put: (aBlock value: (self at: i)).\n    i := i + 1\n  ].\n  ^ res" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "indexOf: anObject\n  | i n |\n  i := 1.\n  n := self size.\n  [ i <= n ] whileTrue: [\n    (self at: i) = anObject ifTrue: [ ^ i ].\n    i := i + 1\n  ].\n  ^ 0" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "reversed\n  | n res i |\n  n := self size.\n  res := self class new: n.\n  i := 1.\n  [ i <= n ] whileTrue: [\n    res at: i put: (self at: (n - i + 1)).\n    i := i + 1\n  ].\n  ^ res" },
  { CLS_SEQUENCEABLE_COLLECTION, false, "asArray\n  | n res i |\n  n := self size.\n  res := Array new: n.\n  i := 1.\n  [ i <= n ] whileTrue: [\n    res at: i put: (self at: i).\n    i := i + 1\n  ].\n  ^ res" },

  // =========================================================================
  // Array, String, Symbol
  // =========================================================================
  { CLS_ARRAY, true, "with: a\n  | r |\n  r := self new: 1.\n  r at: 1 put: a.\n  ^ r" },
  { CLS_ARRAY, true, "with: a with: b\n  | r |\n  r := self new: 2.\n  r at: 1 put: a.\n  r at: 2 put: b.\n  ^ r" },
  { CLS_ARRAY, true, "with: a with: b with: c\n  | r |\n  r := self new: 3.\n  r at: 1 put: a.\n  r at: 2 put: b.\n  r at: 3 put: c.\n  ^ r" },

  { CLS_STRING, false, "asString\n  ^ self" },
  { CLS_STRING, false, "asSymbol\n  <primitive: 64>" },
  { CLS_STRING, false, "asUppercase\n  | n res i |\n  n := self size.\n  res := String new: n.\n  i := 1.\n  [ i <= n ] whileTrue: [\n    res at: i put: (self at: i) asUppercase.\n    i := i + 1\n  ].\n  ^ res" },
  { CLS_STRING, false, "asLowercase\n  | n res i |\n  n := self size.\n  res := String new: n.\n  i := 1.\n  [ i <= n ] whileTrue: [\n    res at: i put: (self at: i) asLowercase.\n    i := i + 1\n  ].\n  ^ res" },

  { CLS_SYMBOL, false, "asSymbol\n  ^ self" },
  { CLS_SYMBOL, false, "asString\n  ^ self copyFrom: 1 to: self size" },

  // =========================================================================
  // OrderedCollection
  // =========================================================================
  { CLS_ORDERED_COLLECTION, false, "initialize\n  array := Array new: 8.\n  tally := 0" },
  { CLS_ORDERED_COLLECTION, false, "size\n  ^ tally" },
  { CLS_ORDERED_COLLECTION, false, "at: index\n  ^ array at: index" },
  { CLS_ORDERED_COLLECTION, false, "at: index put: val\n  ^ array at: index put: val" },
  { CLS_ORDERED_COLLECTION, false, "add: anObject\n  <primitive: 71>" },
  { CLS_ORDERED_COLLECTION, false, "addLast: anObject\n  <primitive: 71>" },
  { CLS_ORDERED_COLLECTION, false, "removeLast\n  <primitive: 72>" },
  { CLS_ORDERED_COLLECTION, false, "do: aBlock\n  | i |\n  i := 1.\n  [ i <= tally ] whileTrue: [\n    aBlock value: (array at: i).\n    i := i + 1\n  ]" },
  { CLS_ORDERED_COLLECTION, false, "asArray\n  | res i |\n  res := Array new: tally.\n  i := 1.\n  [ i <= tally ] whileTrue: [\n    res at: i put: (array at: i).\n    i := i + 1\n  ].\n  ^ res" },
  { CLS_ORDERED_COLLECTION, true,  "new\n  ^ super new initialize" },

  // =========================================================================
  // Dictionary
  // =========================================================================
  { CLS_DICTIONARY, false, "initialize\n  keys := Array new: 8.\n  values := Array new: 8.\n  tally := 0" },
  { CLS_DICTIONARY, false, "size\n  ^ tally" },
  { CLS_DICTIONARY, false, "at: key\n  ^ self at: key ifAbsent: [ nil ]" },
  { CLS_DICTIONARY, false, "at: key ifAbsent: aBlock\n  <primitive: 67>" },
  { CLS_DICTIONARY, false, "at: key put: val\n  <primitive: 68>" },
  { CLS_DICTIONARY, false, "keys\n  <primitive: 69>" },
  { CLS_DICTIONARY, false, "removeKey: key\n  <primitive: 70>" },
  { CLS_DICTIONARY, false, "includesKey: key\n  ^ self keys includes: key" },
  { CLS_DICTIONARY, false, "keysAndValuesDo: aBlock\n  | i |\n  i := 1.\n  [ i <= tally ] whileTrue: [\n    aBlock value: (keys at: i) value: (values at: i).\n    i := i + 1\n  ]" },
  { CLS_DICTIONARY, false, "do: aBlock\n  | i |\n  i := 1.\n  [ i <= tally ] whileTrue: [\n    aBlock value: (values at: i).\n    i := i + 1\n  ]" },
  { CLS_DICTIONARY, true,  "new\n  ^ super new initialize" },

  // =========================================================================
  // Stream, ReadStream, WriteStream
  // =========================================================================
  { CLS_STREAM, false, "on: aCollection\n  collection := aCollection.\n  pos := 0.\n  limit := aCollection size" },
  { CLS_STREAM, false, "position\n  ^ pos" },
  { CLS_STREAM, false, "position: anInt\n  pos := anInt" },
  { CLS_STREAM, false, "atEnd\n  ^ pos >= limit" },
  { CLS_STREAM, false, "contents\n  ^ collection copyFrom: 1 to: limit" },
  { CLS_STREAM, true,  "on: aCollection\n  ^ self new on: aCollection" },

  { CLS_READ_STREAM, false, "next\n  pos >= limit ifTrue: [ ^ nil ].\n  pos := pos + 1.\n  ^ collection at: pos" },
  { CLS_READ_STREAM, false, "peek\n  pos >= limit ifTrue: [ ^ nil ].\n  ^ collection at: (pos + 1)" },

  { CLS_WRITE_STREAM, false, "nextPut: anElement\n  pos := pos + 1.\n  pos > collection size ifTrue: [\n    collection := collection , (String new: 32)\n  ].\n  collection at: pos put: anElement.\n  pos > limit ifTrue: [ limit := pos ].\n  ^ anElement" },
  { CLS_WRITE_STREAM, false, "nextPutAll: aString\n  aString do: [ :ch | self nextPut: ch ].\n  ^ aString" },
  { CLS_WRITE_STREAM, false, "cr\n  ^ self nextPut: (Character value: 10)" },
  { CLS_WRITE_STREAM, false, "space\n  ^ self nextPut: (Character value: 32)" },
  { CLS_WRITE_STREAM, false, "print: anObject\n  ^ self nextPutAll: anObject asString" },

  // =========================================================================
  // Point
  // =========================================================================
  { CLS_POINT, false, "x\n  ^ x" },
  { CLS_POINT, false, "y\n  ^ y" },
  { CLS_POINT, false, "x: a y: b\n  x := a.\n  y := b" },
  { CLS_POINT, false, "+ aPoint\n  ^ Point x: (x + aPoint x) y: (y + aPoint y)" },
  { CLS_POINT, false, "- aPoint\n  ^ Point x: (x - aPoint x) y: (y - aPoint y)" },
  { CLS_POINT, false, "= aPoint\n  ^ (x = aPoint x) and: [ y = aPoint y ]" },
  { CLS_POINT, true,  "x: a y: b\n  ^ self new x: a y: b" },

  // =========================================================================
  // Class, CompiledMethod, BlockClosure, Context
  // =========================================================================
  { CLS_CLASS, false, "new\n  <primitive: 50>" },
  { CLS_CLASS, false, "basicNew\n  <primitive: 50>" },
  { CLS_CLASS, false, "new: size\n  <primitive: 51>" },
  { CLS_CLASS, false, "basicNew: size\n  <primitive: 51>" },
  { CLS_CLASS, false, "name\n  <primitive: 52>" },
  { CLS_CLASS, false, "superclass\n  <primitive: 53>" },
  { CLS_CLASS, false, "subclasses\n  <primitive: 54>" },
  { CLS_CLASS, false, "selectors\n  <primitive: 55>" },
  { CLS_CLASS, false, "classSelectors\n  <primitive: 56>" },
  { CLS_CLASS, false, "sourceCodeAt: aSelector\n  <primitive: 57>" },
  { CLS_CLASS, false, "compile: sourceString\n  <primitive: 58>" },
  { CLS_CLASS, false, "compileClassMethod: sourceString\n  <primitive: 59>" },
  { CLS_CLASS, false, "subclass: aSymbol instanceVariableNames: ivarString\n  <primitive: 60>" },
  { CLS_CLASS, false, "subclass: aSymbol instanceVariableNames: ivarString classVariableNames: cvarString\n  <primitive: 61>" },
  { CLS_CLASS, false, "canUnderstand: aSymbol\n  | c |\n  c := self.\n  [ c notNil ] whileTrue: [\n    (c selectors includes: aSymbol) ifTrue: [ ^ true ].\n    c := c superclass\n  ].\n  ^ false" },

  { CLS_COMPILED_METHOD, false, "selector\n  ^ self instVarAt: 1" },
  { CLS_COMPILED_METHOD, false, "methodClass\n  ^ self instVarAt: 2" },

  { CLS_BLOCK_CLOSURE, false, "value\n  <primitive: 80>" },
  { CLS_BLOCK_CLOSURE, false, "value: arg1\n  <primitive: 81>" },
  { CLS_BLOCK_CLOSURE, false, "value: arg1 value: arg2\n  <primitive: 82>" },
  { CLS_BLOCK_CLOSURE, false, "value: arg1 value: arg2 value: arg3\n  <primitive: 83>" },
  { CLS_BLOCK_CLOSURE, false, "whileTrue: aBlock\n  self value ifTrue: [ aBlock value. self whileTrue: aBlock ].\n  ^ nil" },
  { CLS_BLOCK_CLOSURE, false, "whileFalse: aBlock\n  self value ifFalse: [ aBlock value. self whileFalse: aBlock ].\n  ^ nil" },
  { CLS_BLOCK_CLOSURE, false, "whileTrue\n  ^ self whileTrue: []" },
  { CLS_BLOCK_CLOSURE, false, "whileFalse\n  ^ self whileFalse: []" },
  { CLS_BLOCK_CLOSURE, false, "repeat\n  ^ [ true ] whileTrue: [ self value ]" },
  { CLS_BLOCK_CLOSURE, false, "ensure: aBlock\n  | r |\n  r := self value.\n  aBlock value.\n  ^ r" },

  { CLS_CONTEXT, false, "sender\n  ^ self instVarAt: 1" },
  { CLS_CONTEXT, false, "receiver\n  ^ self instVarAt: 2" },
  { CLS_CONTEXT, false, "method\n  ^ self instVarAt: 3" },
  { CLS_CONTEXT, false, "pc\n  ^ self instVarAt: 6" },

  // =========================================================================
  // Transcript, System, File, Display, Pen, Workspace, ClassBrowser, FileBrowser
  // =========================================================================
  { CLS_TRANSCRIPT, true,  "show: anObject\n  <primitive: 90>" },
  { CLS_TRANSCRIPT, true,  "print: anObject\n  self show: anObject printString" },
  { CLS_TRANSCRIPT, true,  "cr\n  <primitive: 91>" },
  { CLS_TRANSCRIPT, true,  "clear\n  <primitive: 92>" },
  { CLS_TRANSCRIPT, true,  "contents\n  <primitive: 93>" },

  { CLS_SYSTEM, true,  "gc\n  <primitive: 94>" },
  { CLS_SYSTEM, true,  "freeHeap\n  <primitive: 95>" },
  { CLS_SYSTEM, true,  "millis\n  <primitive: 96>" },
  { CLS_SYSTEM, true,  "delayMs: ms\n  <primitive: 97>" },
  { CLS_SYSTEM, true,  "ledR: r g: g b: b ms: ms\n  <primitive: 98>" },
  { CLS_SYSTEM, true,  "classes\n  <primitive: 99>" },

  { CLS_FILE, true,  "ls: path\n  <primitive: 100>" },
  { CLS_FILE, true,  "read: path\n  <primitive: 101>" },
  { CLS_FILE, true,  "write: path contents: str\n  <primitive: 102>" },
  { CLS_FILE, true,  "exists: path\n  <primitive: 103>" },
  { CLS_FILE, true,  "delete: path\n  <primitive: 104>" },
  { CLS_FILE, true,  "mkdir: path\n  <primitive: 105>" },

  { CLS_DISPLAY, true,  "clear: color\n  <primitive: 110>" },
  { CLS_DISPLAY, true,  "fillRectX: x y: y w: w h: h color: c\n  <primitive: 111>" },
  { CLS_DISPLAY, true,  "drawLineX0: x0 y0: y0 x1: x1 y1: y1 color: c\n  <primitive: 112>" },
  { CLS_DISPLAY, true,  "drawString: s x: x y: y color: c\n  <primitive: 113>" },

  { CLS_PEN, false, "initialize\n  x := 160.\n  y := 130.\n  angle := 0.\n  down := true.\n  color := 2047" },
  { CLS_PEN, false, "place: px y: py\n  x := px.\n  y := py" },
  { CLS_PEN, false, "gotoX: px y: py\n  x := px.\n  y := py" },
  { CLS_PEN, false, "up\n  down := false" },
  { CLS_PEN, false, "down\n  down := true" },
  { CLS_PEN, false, "color: c\n  color := c" },
  { CLS_PEN, false, "turn: deg\n  angle := angle + deg" },
  { CLS_PEN, false, "go: dist\n  <primitive: 114>" },
  { CLS_PEN, true,  "new\n  ^ super new initialize" },

  { CLS_WORKSPACE, false, "open\n  <primitive: 120>" },
  { CLS_WORKSPACE, false, "openWith: text\n  <primitive: 120>" },
  { CLS_WORKSPACE, true,  "open\n  ^ self new open" },
  { CLS_WORKSPACE, true,  "openWith: text\n  ^ self new openWith: text" },

  { CLS_CLASS_BROWSER, false, "open\n  <primitive: 121>" },
  { CLS_CLASS_BROWSER, false, "openOn: aClass\n  <primitive: 121>" },
  { CLS_CLASS_BROWSER, true,  "open\n  ^ self new open" },
  { CLS_CLASS_BROWSER, true,  "openOn: aClass\n  ^ self new openOn: aClass" },

  { CLS_FILE_BROWSER, false, "open\n  <primitive: 122>" },
  { CLS_FILE_BROWSER, false, "openOn: path\n  <primitive: 122>" },
  { CLS_FILE_BROWSER, true,  "open\n  ^ self new open" },
  { CLS_FILE_BROWSER, true,  "openOn: path\n  ^ self new openOn: path" },
};

static constexpr int kNumBootstrapMethods =
    (int)(sizeof(kBootstrapMethods) / sizeof(kBootstrapMethods[0]));

const char* stGetBootstrapSource(int idx) {
  if (idx < 0 || idx >= kNumBootstrapMethods) return nullptr;
  return kBootstrapMethods[idx].source;
}

// Patches `BlockClosure>>whileTrue:` and `BlockClosure>>whileFalse:` with an
// O(1)-space backward-jump loop so arbitrary loops never grow the Context stack.
static void patchBlockLoopBytecode(VMState* vm, Oop blkCls, const char* selStr, uint8_t condJumpOp) {
  Oop idict = stSlotAt(vm, blkCls, SLOT_CLS_INST_METHODS);
  Oop meth  = stDictAt(vm, idict, stInternSymbol(vm, selStr), kOopNull);
  if (meth == kOopNull || meth == kOopNil) return;

  Oop lits = stNewArray(vm, 1);
  stSlotAtPut(vm, lits, 0, vm->symValue);

  // 0: OP_PUSH_SELF
  // 1: OP_SEND 0 (#value), 0
  // 4: condJumpOp +9 (to 16)
  // 7: OP_PUSH_TEMP 0 (aBlock)
  // 9: OP_SEND 0 (#value), 0
  // 12: OP_POP
  // 13: OP_JUMP -16 (0xFFF0 -> back to 0)
  // 16: OP_PUSH_NIL
  // 17: OP_RETURN_TOP
  const uint8_t loopBc[18] = {
      OP_PUSH_SELF,
      OP_SEND, 0, 0,
      condJumpOp, 9, 0,
      OP_PUSH_TEMP, 0,
      OP_SEND, 0, 0,
      OP_POP,
      OP_JUMP, 0xF0, 0xFF,
      OP_PUSH_NIL,
      OP_RETURN_TOP
  };
  Oop bcArr = stNewBytes(vm, CLS_BYTE_ARRAY, FMT_BYTES, loopBc, sizeof(loopBc));
  stSlotAtPut(vm, meth, SLOT_METH_LITERALS,  lits);
  stSlotAtPut(vm, meth, SLOT_METH_BYTECODES, bcArr);
}

void stBootstrapInstall(VMState* vm) {
  vm->pinNewObjects = true;

  // 1. Create core class hierarchy in topological order
  Oop cObj     = stCreateClass(vm, CLS_OBJECT,                  "Object",                 kOopNil, "");
  Oop cUndef   = stCreateClass(vm, CLS_UNDEFINED_OBJECT,        "UndefinedObject",        cObj,    "");
  Oop cBool    = stCreateClass(vm, CLS_BOOLEAN,                 "Boolean",                cObj,    "");
  Oop cTrue    = stCreateClass(vm, CLS_TRUE,                    "True",                   cBool,   "");
  Oop cFalse   = stCreateClass(vm, CLS_FALSE,                   "False",                  cBool,   "");
  Oop cMag     = stCreateClass(vm, CLS_MAGNITUDE,               "Magnitude",              cObj,    "");
  Oop cNum     = stCreateClass(vm, CLS_NUMBER,                  "Number",                 cMag,    "");
  Oop cSmi     = stCreateClass(vm, CLS_SMALL_INTEGER,           "SmallInteger",           cNum,    "");
  Oop cFlt     = stCreateClass(vm, CLS_FLOAT,                   "Float",                  cNum,    "");
  Oop cChar    = stCreateClass(vm, CLS_CHARACTER,               "Character",              cMag,    "");
  Oop cColl    = stCreateClass(vm, CLS_COLLECTION,              "Collection",             cObj,    "");
  Oop cSeq     = stCreateClass(vm, CLS_SEQUENCEABLE_COLLECTION, "SequenceableCollection", cColl,   "");
  Oop cArr     = stCreateClass(vm, CLS_ARRAY,                   "Array",                  cSeq,    "");
  Oop cByteArr = stCreateClass(vm, CLS_BYTE_ARRAY,              "ByteArray",              cSeq,    "");
  Oop cStr     = stCreateClass(vm, CLS_STRING,                  "String",                 cSeq,    "");
  Oop cSym     = stCreateClass(vm, CLS_SYMBOL,                  "Symbol",                 cStr,    "");
  Oop cOrdColl = stCreateClass(vm, CLS_ORDERED_COLLECTION,      "OrderedCollection",      cSeq,    "array tally");
  Oop cDict    = stCreateClass(vm, CLS_DICTIONARY,              "Dictionary",             cColl,   "keys values tally");
  Oop cStream  = stCreateClass(vm, CLS_STREAM,                  "Stream",                 cObj,    "collection pos limit");
  Oop cRStream = stCreateClass(vm, CLS_READ_STREAM,             "ReadStream",             cStream, "");
  Oop cWStream = stCreateClass(vm, CLS_WRITE_STREAM,            "WriteStream",            cStream, "");
  Oop cClass   = stCreateClass(vm, CLS_CLASS,                   "Class",                  cObj,    "name superclass instanceMethods classMethods instVarNames instSize classVars");
  Oop cMeth    = stCreateClass(vm, CLS_COMPILED_METHOD,         "CompiledMethod",         cObj,    "selector ownerClass literals bytecodes header source");
  Oop cBlk     = stCreateClass(vm, CLS_BLOCK_CLOSURE,           "BlockClosure",           cObj,    "homeContext outerContext method startPc numArgs tempBase");
  Oop cCtx     = stCreateClass(vm, CLS_CONTEXT,                 "Context",                cObj,    "sender receiver method closure home pc sp");
  Oop cSys     = stCreateClass(vm, CLS_SYSTEM,                  "System",                 cObj,    "");
  Oop cTrans   = stCreateClass(vm, CLS_TRANSCRIPT,              "Transcript",             cObj,    "");
  Oop cFile    = stCreateClass(vm, CLS_FILE,                    "File",                   cObj,    "");
  Oop cDisp    = stCreateClass(vm, CLS_DISPLAY,                 "Display",                cObj,    "");
  Oop cPen     = stCreateClass(vm, CLS_PEN,                     "Pen",                    cObj,    "x y angle down color");
  Oop cWork    = stCreateClass(vm, CLS_WORKSPACE,               "Workspace",              cObj,    "");
  Oop cCBrow   = stCreateClass(vm, CLS_CLASS_BROWSER,           "ClassBrowser",           cObj,    "");
  Oop cFBrow   = stCreateClass(vm, CLS_FILE_BROWSER,            "FileBrowser",            cObj,    "");
  Oop cPoint   = stCreateClass(vm, CLS_POINT,                   "Point",                  cObj,    "x y");

  (void)cUndef; (void)cTrue; (void)cFalse; (void)cSmi; (void)cFlt;
  (void)cChar; (void)cArr; (void)cByteArr; (void)cSym; (void)cOrdColl;
  (void)cDict; (void)cRStream; (void)cWStream; (void)cClass; (void)cMeth;
  (void)cCtx; (void)cSys; (void)cTrans; (void)cFile; (void)cDisp;
  (void)cPen; (void)cWork; (void)cCBrow; (void)cFBrow; (void)cPoint;

  // 2. Compile and install all bootstrap methods from `.rodata`
  for (int i = 0; i < kNumBootstrapMethods; i++) {
    const BootstrapMethodDef& def = kBootstrapMethods[i];
    Oop targetCls = vm->classes[def.clsId];
    stCompileMethod(vm, targetCls, def.isClassMethod, def.source, i);
  }

  // 3. Patch `BlockClosure>>whileTrue:` and `whileFalse:` with O(1)-context loop bytecode
  patchBlockLoopBytecode(vm, cBlk, "whileTrue:",  OP_JUMP_IF_FALSE);
  patchBlockLoopBytecode(vm, cBlk, "whileFalse:", OP_JUMP_IF_TRUE);

  vm->pinNewObjects = false;
  stRunGC(vm);
}
