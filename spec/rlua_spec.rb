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
  end
end
