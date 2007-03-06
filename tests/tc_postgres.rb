require 'test/unit'

class PostgresTestCase < Test::Unit::TestCase

  def setup
    PGconn.translate_results = true
    @conn = PGconn.new('dbname' => 'template1')
  end

  def teardown
    @conn.close
  end

  def test_conversion
    query = <<-EOT
select true as true_value,
       false as false_value,
       $1::bytea as bytea_value,
       '2005-11-30'::date as date_value,
       '12:00:00'::time as time_value,
       '2005-11-30 12:00:00'::timestamp as date_time_value,
       1.5::float as float_value,
       12345.5678::numeric as numeric_value,
       1234.56::numeric(10) as numeric_10_value,
       12345.12345::numeric(10,5) as numeric_10_5_value
EOT
    res = @conn.exec(query, "12345\0TEST")
    assert_equal(1, res.num_tuples, 1)
    assert_equal(10, res.num_fields, 10)
    tuple = res.result.first
    assert_equal(true, tuple['true_value'])
    assert_equal(false, tuple['false_value'])
    assert_equal("12345\0TEST", tuple['bytea_value'])
    assert_equal(Date.parse('2005-11-30'), tuple['date_value'])
    assert_equal(Time.parse('12:00:00'), tuple['time_value'])
    assert_equal(Time.parse('2005-11-30 12:00:00'), tuple['date_time_value'])
    assert_equal(1.5, tuple['float_value'])
    assert_equal(BigDecimal("12345.5678"), tuple['numeric_value'])
    assert_equal(1235, tuple['numeric_10_value'])
    assert_kind_of(Integer, tuple['numeric_10_value'])
    assert_equal(BigDecimal("12345.12345"), tuple['numeric_10_5_value'])
  end

  def test_select_one
    res = @conn.select_one("select 1 as a,2 as b union select 2 as a,3 as b order by 1")
    assert_equal([1,2], res)
  end

  def test_select_values
    res = @conn.select_values("select 1,2 union select 2,3 order by 1")
    assert_equal([1,2,2,3], res)
  end

  def test_select_value
    res = @conn.select_value("select 'test', 123")
    assert_equal("test", res)
  end

  def test_row_each
    res = @conn.exec("select 1 as a union select 2 as a union select 3 as a order by 1")
    n = 1
    res.each do |tuple|
      assert_equal(n, tuple['a'])
      n +=1
    end
  end

end
