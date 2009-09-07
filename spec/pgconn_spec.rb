require 'rubygems'
require 'spec'
require File.join(File.dirname(__FILE__), 'spec_helper')

$LOAD_PATH.unshift('ext')
require 'pg'

describe PGconn do

	before( :all ) do
		puts "======  TESTING PGconn  ======"
		@test_directory = ENV['TMP'] || ENV['TEMP'] || "/tmp"
		unless @already_running = server_running?
			server_build
			server_start
		end

		@host = PGSQL_INF['host']
		@port = PGSQL_INF['port']
		@user = PGSQL_INF['user']
		@pswd = PGSQL_INF['password']
		@dbname = PGSQL_INF['dbname']

		@conninfo = ''
		@conninfo += "host=#{@host} " if @host
		@conninfo += "port=#{@port} " if @port
		@conninfo += "dbname=#{@dbname} " if @dbname
		@conninfo += "user=#{@user} " if @user
		@conninfo += "password=#{@password} " if @password

		@conn = PGconn.connect(@conninfo)
	end

	it "should connect successfully with connection string" do
		tmpconn = PGconn.connect(@conninfo)
		tmpconn.status.should== PGconn::CONNECTION_OK
		tmpconn.finish
	end

	it "should connect using 7 arguments converted to strings" do
		tmpconn = PGconn.connect(@host, @port, nil, nil, @dbname, @user, @password)
		tmpconn.status.should== PGconn::CONNECTION_OK
		tmpconn.finish
	end

	it "should connect using hash" do
		tmpconn = PGconn.connect(
			:host => @host,
			:port => @port,
			:dbname => @dbname,
			:user => @user,
			:password => @password)
		tmpconn.status.should== PGconn::CONNECTION_OK
		tmpconn.finish
	end

	it "should connect asynchronously" do
		tmpconn = PGconn.connect_start(@conninfo)
		socket = IO.for_fd(tmpconn.socket)
		status = tmpconn.connect_poll
		while(status != PGconn::PGRES_POLLING_OK) do
			if(status == PGconn::PGRES_POLLING_READING)
				if(not select([socket],[],[],5.0))
					raise "Asynchronous connection timed out!"
				end
			elsif(status == PGconn::PGRES_POLLING_WRITING)
				if(not select([],[socket],[],5.0))
					raise "Asynchronous connection timed out!"
				end
			end
			status = tmpconn.connect_poll
		end
		tmpconn.status.should== PGconn::CONNECTION_OK
		tmpconn.finish
	end

	it "should not leave stale server connections after finish" do
		before = @conn.exec(%[SELECT COUNT(*) AS n FROM pg_stat_activity WHERE usename IS NOT NULL])

		PGconn.connect(@conninfo).finish
		sleep 0.5

		after = @conn.exec(%[SELECT COUNT(*) AS n FROM pg_stat_activity WHERE usename IS NOT NULL])

		# there's still the global @conn, but should be no more
		before[0]['n'].should == after[0]['n']
	end

	unless RUBY_PLATFORM =~ /mswin|mingw/
		it "should trace and untrace client-server communication" do
			# be careful to explicitly close files so that the
			# directory can be removed and we don't have to wait for
			# the GC to run.

			expected_trace_file = File.join(Dir.getwd, "spec/data", "expected_trace.out")
			expected_trace_data = open(expected_trace_file, 'rb').read
			trace_file = open(File.join(@test_directory, "test_trace.out"), 'wb')
			@conn.trace(trace_file)
			trace_file.close
			res = @conn.exec("SELECT 1 AS one")
			@conn.untrace
			res = @conn.exec("SELECT 2 AS two")
			trace_file = open(File.join(@test_directory, "test_trace.out"), 'rb')
			trace_data = trace_file.read
			trace_file.close
			trace_data.should == expected_trace_data
		end
	end

	it "should cancel a query" do
		@conn.setnonblocking(true).should be_nil
		@conn.isnonblocking.should be_true
		@conn.send_query("SELECT pg_sleep(10)").should be_nil
		@conn.cancel.should be_nil
		tmpres = @conn.get_result
		(tmpres.result_status != PGresult::PGRES_TUPLES_OK).should be_true
	end

	after( :all ) do
		puts ""
		@conn.finish
		unless @already_running
			server_stop
			server_clean
		end
		puts "====== COMPLETED TESTING PGconn  ======"
		puts ""
	end
end
