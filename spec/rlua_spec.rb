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

      expect { subject.__eval "ruby:zaphod()" }.to output("\"Meaning of life: 42.0\"\n").to_stdout
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

  end
end
