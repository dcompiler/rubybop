#require 'math'
#require 'test/unit/assertions.rb'

#require 'bop_api.h'

class BOP_add

  def main()
    if (ARGV.length>3 || ARGV.length<2 )
      abort("Usage: %s array_size-in-millions num-blocks", ARGV[0])
    end

    @data_size = (ARGV[0].to_f)*1000000
    @num_blocks = ARGV[1].to_i

    init(@data_size)

    puts"#{Process.pid}: adding #{(@data_size/1000)} million numbers"
    @block_size = ( @data_size / @num_blocks ).ceil

    while ( @data_size > 0 )
      @block_end = @data_size
      @data_size -= @block_size
      @block_begin = @data_size >= 0 ? @data_size : 0

        @block_sum = lots_of_computation_on_block( @block_begin, @block_end )
          $sum += @block_sum


    end #end while

    PPR.over
    puts "#{Process.pid}: The sum is #{($sum/1000000.0)} million (#{$sum.to_f}) "

  end #end main

  def init( data_size )
    puts "#{Process.pid}: initializing #{(@data_size/1000000)} million numbers"
    $data = Array.new(data_size, 0)
    $sum = 0;

  end

  def lots_of_computation_on_block( s, e )
    @total = 0;
    j = s
    while j < e do
      @total += ($data[j]**10) + ($data[j]**20) - ($data[j]**30) + 1
      j += 1
    end
    return @total;
  end

end #class end

run = BOP_add.new
run.main
