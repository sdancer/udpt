defmodule UdClient do
  use GenServer

  def start() do
    GenServer.start Udptun, []
  end

  def init(_) do
    :ets.new :clients, [:named_table]
    #listen a tcp port
    {:ok, port} = :gen_udp.open 8888, [mode: :binary]
    IO.inspect port
    {:ok, port}
  end

  def tcpport() do
    #start new clients as pasive and don't read

    #...
    #when a tcpclient connects, send server a MSG_TYPE_HELLO
    #on ack, send a new ack

    #once the tunel has been ack'ed, read and change to active
  end


  #on input data, queue
  
end
