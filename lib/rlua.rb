#  This file is part of RLua.
#
#  RLua is free software: you can redistribute it and/or modify
#  it under the terms of the GNU Lesser General Public License as
#  published by the Free Software Foundation, either version 3 of
#  the License, or (at your option) any later version.
#
#  RLua is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public
#  License along with RLua.  If not, see <http://www.gnu.org/licenses/>.

require 'rlua.so'

module Lua
  class Table
    # Traverses the table using Lua::Table.next function.
    def each(&block)
      key = nil
      loop {
        key, value = *self.class.next(self, key)
        break if key.nil?
        block.call(key, value)
      }
      self
    end

    # Non-recursively converts table to hash.
    def to_hash
      hash = {}
      each { |k, v| hash[k] = v }
      hash
    end

    # Converts table to array. Only integer indexes are used.
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

    # Recursively pretty-prints the table properly handling reference loops.
    #
    # +trace+ argument is internal, do not use it.
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
