# encoding: utf-8

require 'rubygems'
require 'spec'

$LOAD_PATH.unshift('ext')
require 'pg'

describe PGconn do

	before( :all ) do
		puts "======  TESTING PGresult  ======"
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

	it "should act as an array of hashes" do
		res = @conn.exec("SELECT 1 AS a, 2 AS b")
		res[0]['a'].should== '1'
		res[0]['b'].should== '2'
	end

	it "should insert nil AS NULL and return NULL as nil" do
		res = @conn.exec("SELECT $1::int AS n", [nil])
		res[0]['n'].should == nil
	end

	it "should detect division by zero as SQLSTATE 22012" do
		sqlstate = nil
		begin
			res = @conn.exec("SELECT 1/0")
		rescue PGError => e
			sqlstate = e.result.result_error_field(
				PGresult::PG_DIAG_SQLSTATE).to_i
		end
		sqlstate.should == 22012
	end

	it "should return the same bytes in binary format that are sent in binary format" do
		binary_file = File.join(Dir.pwd, 'spec/data', 'random_binary_data')
		bytes = File.open(binary_file, 'rb').read
		res = @conn.exec('VALUES ($1::bytea)',
			[ { :value => bytes, :format => 1 } ], 1)
		res[0]['column1'].should== bytes
	end

	it "should return the same bytes in binary format that are sent as inline text" do
		binary_file = File.join(Dir.pwd, 'spec/data', 'random_binary_data')
		in_bytes = File.open(binary_file, 'rb').read
		out_bytes = nil
		@conn.transaction do |conn|
			conn.exec("SET standard_conforming_strings=on")
			res = conn.exec("VALUES ('#{PGconn.escape_bytea(in_bytes)}'::bytea)", [], 1)
			out_bytes = res[0]['column1']
		end
		out_bytes.should== in_bytes
	end

	it "should return the same bytes in text format that are sent in binary format" do
		binary_file = File.join(Dir.pwd, 'spec/data', 'random_binary_data')
		bytes = File.open(binary_file, 'rb').read
		res = @conn.exec('VALUES ($1::bytea)',
			[ { :value => bytes, :format => 1 } ])
		PGconn.unescape_bytea(res[0]['column1']).should== bytes
	end

	it "should return the same bytes in text format that are sent as inline text" do
		binary_file = File.join(Dir.pwd, 'spec/data', 'random_binary_data')
		in_bytes = File.open(binary_file, 'rb').read

		out_bytes = nil
		@conn.transaction do |conn|
			conn.exec("SET standard_conforming_strings=on")
			res = conn.exec("VALUES ('#{PGconn.escape_bytea(in_bytes)}'::bytea)", [], 0)
			out_bytes = PGconn.unescape_bytea(res[0]['column1'])
		end
		out_bytes.should== in_bytes
	end

	after( :all ) do
		puts ""
		@conn.finish
		unless @already_running
			server_stop
			server_clean
		end
		puts "======  COMPLETED TESTING PGresult  ======"
		puts ""
	end
end
