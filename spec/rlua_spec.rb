# encoding: utf-8
require 'rlua'

describe Lua::State do
  context 'ruby' do
    it 'creates a variable in Ruby and pass it to Lua' do
      subject.value = 10

      subject.__eval 'value = value * 2'

      expect(subject.value).to eq 20
    end

    it 'creates a table in Ruby and pass it to Lua' do
      subject.ruby = {
        'meaning_of_life' => 42,
        'zaphod' => lambda { |table| p "Meaning of life: #{table.meaning_of_life}" }
      }

      expect { subject.__eval "ruby:zaphod()" }.to output("\"Meaning of life: 42\"\n").to_stdout
    end

    describe 'type conversions to lua' do
      before do
        subject.__load_stdlib(:base, :math)
      end

      it 'converts String to string' do
        subject.value = "hello"
        subject.__eval 't = type(value)'
        expect(subject.t).to eq("string")
      end

      it 'converts Array to table' do
        subject.value = [1,2,3]
        subject.__eval 't = type(value)'
        expect(subject.t).to eq("table")
      end

      it 'converts Hash to table' do
        subject.value = {'a' => 1, 'b' => 2}
        subject.__eval 't = type(value)'
        expect(subject.t).to eq("table")
      end

      it 'converts NilClass to nil' do
        subject.value = nil
        subject.__eval 't = type(value)'
        expect(subject.t).to eq("nil")
      end

      it 'converts TrueClass to boolean' do
        subject.value = true
        subject.__eval 't = type(value)'
        expect(subject.t).to eq("boolean")
      end

      it 'converts FalseClass to boolean' do
        subject.value = false
        subject.__eval 't = type(value)'
        expect(subject.t).to eq("boolean")
      end

      it 'converts small Integer to integer' do
        subject.value = 1
        subject.__eval 't = math.type(value)'
        expect(subject.t).to eq("integer")
      end

      it 'converts large Integer to float' do
        subject.value = 2**64
        subject.__eval 't = math.type(value)'
        expect(subject.t).to eq("float")
      end

      it 'converts Float to float' do
        subject.value = 1.0
        subject.__eval 't = math.type(value)'
        expect(subject.t).to eq("float")
      end

      it 'converts Proc to function' do
        subject.value = ->(x){ x }
        subject.__eval 't = type(value)'
        expect(subject.t).to eq("function")
      end
    end
  end

  describe 'lua' do
    it 'creates a variable in Lua and pass it to Ruby' do
      subject.__eval 'value = 15'

      expect(subject.value).to eq 15
    end

    it 'creates a function in Lua and launch it from Ruby' do
      subject.__eval "ran = false"
      subject.__eval "function lua_func() ran = true end"

      expect(subject.ran).to be_falsey

      subject.lua_func

      expect(subject.ran).to be_truthy
    end

    context 'string encoding' do
      before { Encoding.default_external = Encoding::UTF_8 }

      it 'creates a string in Lua and pass it to Ruby with default_external encoding' do
        subject.__eval 'value = "höhöhö"'

        expect(subject.value).to eq 'höhöhö'
        expect(subject.value.encoding).to eq Encoding::UTF_8
      end

      it 'creates a string in Lua and pass it to Ruby with custom default_external encoding' do
        Encoding.default_external = Encoding::ISO8859_15

        subject.__eval 'value = "höhöhö"'.encode(Encoding::ISO8859_15)

        expect(subject.value).to eq 'höhöhö'.encode(Encoding::ISO8859_15)
        expect(subject.value.encoding).to eq Encoding::ISO8859_15
      end

      it 'creates a string in Lua and pass it to Ruby with default_external encoding' do
        subject.value = 'höhöhö'.encode(Encoding::EUCJP_MS)

        expect(subject.value).to eq 'höhöhö'
        expect(subject.value.encoding).to eq Encoding.default_external
      end
    end

    context 'bootstrap' do
      before { subject.__bootstrap }

      describe 'tonumber' do
        it 'converts string without specifying a base' do
          subject.__eval 'value = tonumber("42")'

          expect(subject.value).to eq 42
        end

        it 'converts string using specified base' do
          subject.__eval 'value = tonumber("42", 13)'

          expect(subject.value).to eq 54
        end
      end

      describe 'tostring' do
        it 'converts number to string' do
          subject.__eval 'value = tostring(42)'

          expect(subject.value).to eq '42'
        end

        it 'converts string to string' do
          subject.__eval 'value = tostring("zaphod")'

          expect(subject.value).to eq 'zaphod'
        end
      end

      describe 'error' do
        it 'raises a runtime error' do
          expect {
            subject.__eval 'error("bleepbloop")'
          }.to raise_error(RuntimeError, "<eval>:1: bleepbloop")
        end
      end

      describe 'type' do
        it 'identifies strings' do
          subject.__eval 'value = type("zaphod")'

          expect(subject.value).to eq 'string'
        end

        it 'identifies booleans' do
          subject.__eval 'value = type(true)'

          expect(subject.value).to eq 'boolean'
        end

        it 'identifies numbers' do
          subject.__eval 'value = type(42)'

          expect(subject.value).to eq 'number'
        end

        it 'identifies tables' do
          subject.__eval 'value = type({"trillian"})'

          expect(subject.value).to eq 'table'
        end
      end
    end

    describe 'type conversions to ruby' do
      before do
        subject.__load_stdlib(:base, :math)
      end

      it 'converts string to String' do
        subject.__eval 'value = "hello"'
        expect(subject.value.class).to eq(String)
      end

      it 'converts table to Lua::Table' do
        subject.__eval 'value = {1,2,3}'
        expect(subject.value.class).to eq(Lua::Table)
      end

      it 'converts table to Lua::Table' do
        subject.__eval 'value = { a = 1, b = 2 }'
        expect(subject.value.class).to eq(Lua::Table)
      end

      it 'converts nil to NilClass' do
        subject.__eval 'value = { a = nil }'
        expect(subject.value["a"].class).to eq(NilClass)
      end

      it 'converts true to TrueClass' do
        subject.__eval 'value = true'
        expect(subject.value.class).to eq(TrueClass)
      end

      it 'converts false to FalseClass' do
        subject.__eval 'value = false'
        expect(subject.value.class).to eq(FalseClass)
      end

      it 'converts integer to Integer' do
        subject.__eval 'value = 1'
        expect(subject.value.integer?).to be(true)
      end

      it 'converts float to Float' do
        subject.__eval 'value = 1.0'
        expect(subject.value.class).to eq(Float)
      end

      it 'converts function to Lua::Function' do
        subject.__eval 'value = { a = function(x) print(x) end }'
        expect(subject.value["a"].class).to eq(Lua::Function)
      end
    end

    context '__load_stdlib' do
      it "creates a global name for most libraries" do
        [:math, :table, :utf8, :io, :os, :package].each do |lib|
          subject.__load_stdlib(lib)
          subject.__eval "value = (#{lib} == nil)"
          expect(subject.value).to eq(false)
        end
      end

      it "does not create a global name when loading base" do
        subject.__load_stdlib("base")
        subject.__eval "value = (base == nil)"
        expect(subject.value).to eq(true)
      end
    end
  end
end
