require 'rlua.so'

module Lua
  class Table
    def each(&block)
      key = nil
      loop {
        key, value = *self.class.next(self, key)
        break if key.nil?
        block.call(key, value)
      }
      self
    end
    
    def to_hash
      hash = {}
      each { |k, v| hash[k] = v }
      hash
    end
    
    def to_ary
      ary = []
      1.upto(__length) { |i|
        if self[i]
          ary << self[i]
        else
          break
        end
      }
      ary
    end
    alias :to_a :to_ary
    
    def inspect(trace=[])
      if to_hash.empty?
        "L{}"
      elsif trace.include? self
        "L{...}"
      else
        trace << self
        v = []
        each { |key, value|
          s = ""
          if key.class == self.class
            s += key.inspect(trace)
          else
            s += key.inspect
          end
          s += " => "
          if value.class == self.class
            s += value.inspect(trace)
          else
            s += value.inspect
          end
          v << s
        }
        "L{#{v.join ', '}}"
      end
    end
  end
end